/*
 * Copyright (C) 2020, Xilinx Inc - All rights reserved
 * Xilinx Runtime (XRT) Experimental APIs
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <google/protobuf/text_format.h>
#include <glog/logging.h>
/* header file OpenCV for image processing */
#include <opencv2/opencv.hpp>
/* header file for Runner APIs */
#include <vart/runner.hpp>
#include <vart/runner_ext.hpp>
#include <vitis/ai/nnpp/tfssd.hpp>
#include <vitis/ai/nnpp/ssd.hpp>
#include <vitis/ai/profiling.hpp>
#include <xir/graph/graph.hpp>
#include <xir/tensor/tensor.hpp>

/* HW post proc-sort Init */
#include "tfssd/sort_xrt/sort_wrapper.h"
PPHandle *prehandle;
PPHandle *posthandle;

using namespace std;
using namespace cv;
using namespace std::chrono;

std::vector<const xir::Subgraph *> get_dpu_subgraph(
	const xir::Graph *graph)
{
	auto root = graph->get_root_subgraph();
	auto children = root->children_topological_sort();
	auto ret = std::vector<const xir::Subgraph *>();
	for (auto c : children)
	{
		CHECK(c->has_attr("device"));
		auto device = c->get_attr<std::string>("device");
		if (device == "DPU")
		{
			ret.emplace_back(c);
		}
	}
	return ret;
}

std::string slurp(const char *filename);
vitis::ai::proto::DpuModelParam get_config(std::string config_file)
{
	vitis::ai::proto::DpuModelParamList mlist;
	auto text = slurp(config_file.c_str());
	auto ok = google::protobuf::TextFormat::ParseFromString(text, &mlist);
	CHECK(ok) << "cannot parse config file. config_file=" << config_file;
	CHECK_EQ(mlist.model_size(), 1)
		<< "only support one model per config file."
		<< "config_file " << config_file << " "		 //
		<< "content: " << mlist.DebugString() << " " //
		;
	return mlist.model(0);
}
std::string slurp(const char *filename)
{
	std::ifstream in;
	in.open(filename, std::ifstream::in);
	CHECK(in.good()) << "failed to read config file. filename=" << filename;
	std::stringstream sstr;
	sstr << in.rdbuf();
	in.close();
	return sstr.str();
}

void central_crop(const Mat &image, int height, int width, Mat &img)
{
	int offset_h = (image.rows - height) / 2;
	int offset_w = (image.cols - width) / 2;
	Rect box(offset_w, offset_h, width, height);
	img = image(box);
}

void preProcessSsdmobilenet(Mat &image, float scale, int width, int height, int num_channels_, int8_t *data)
{
	cv::Mat img, float_image;
	if (num_channels_ < 3)	{
		cv::cvtColor(image, float_image, cv::COLOR_GRAY2RGB);
	} else	{
		cv::cvtColor(image, float_image, cv::COLOR_BGR2RGB);
	}

	cv::resize((float_image), (img),
			   cv::Size(width, height), cv::INTER_LINEAR);
	int i = 0;
	for (int c = 0; c < 3; c++)	{
		for (int h = 0; h < height; h++) {
			for (int w = 0; w < width; w++)	{
				data[0 + (3 * h * width) + (w * 3) + c] = (int8_t)((img.at<Vec3b>(h, w)[c] * 2 / 255.0 - 1.0) * scale);
			}
		}
	}
	i++;
}

void runSSD(vart::Runner *runner, string config_file, string img_dir, bool profile, bool en_hwpost, bool en_hwpre)
{
	auto outTBuffs_ = dynamic_cast<vart::RunnerExt *>(runner)->get_outputs();
	auto inTBuffs_ = dynamic_cast<vart::RunnerExt *>(runner)->get_inputs();

	uint64_t dpu_input_phy_addr = 0u;
	uint64_t dpu_input_size = 0u;
	std::tie(dpu_input_phy_addr, dpu_input_size) = inTBuffs_[0]->data_phy({0, 0, 0, 0});

	uint64_t dpu_output_phy_addr[2] = {0u, 0u};
	uint64_t dpu_output_size[2] = {0u, 0u};

	int idx = 0;
	for (auto output : outTBuffs_)	{
		std::tie(dpu_output_phy_addr[idx], dpu_output_size[idx]) = output->data_phy({0, 0, 0});
		idx++;
	}

	int8_t *input_ptr = (int8_t *)inTBuffs_[0]->data().first;
	auto ip_scales = vart::get_input_scale(dynamic_cast<vart::RunnerExt *>(runner)->get_input_tensors());
	auto out_scales = vart::get_output_scale(dynamic_cast<vart::RunnerExt *>(runner)->get_output_tensors());
	float loc_scale = out_scales[0];
	float conf_scale = out_scales[1];

	auto tensor = inTBuffs_[0]->get_tensor();
	auto in_dims = tensor->get_shape();
	int height = in_dims[1];
	int width = in_dims[2];
	int channels = in_dims[3];

	vitis::ai::proto::DpuModelParam config_ = get_config(config_file);

	vector<std::unique_ptr<vitis::ai::TFSSDPostProcess>> processor_;
	processor_.push_back(vitis::ai::TFSSDPostProcess::create(
		width, height, conf_scale, loc_scale, config_));

	//# Hardware pre proc kernel init
	if (en_hwpre)
		pp_kernel_init(prehandle, ip_scales[0]);

	//# Hardware post proc kernel init
	if (en_hwpost)	{
		const short *fx_priors_ = processor_[0]->fixed_priors_;
		hw_sort_init(posthandle, fx_priors_);
		posthandle->dpu_box_out_phy_addr = dpu_output_phy_addr[0];
		posthandle->dpu_conf_out_phy_addr = dpu_output_phy_addr[1];
	}

	long imread_time = 0;
	vector<cv::String> files;
	cv::glob(img_dir, files);
	int count = files.size();
	std::cerr << "The image count = " << count << endl;

	auto start = std::chrono::system_clock::now();
	//# Loop Over images
	for (int i = 0; i < count; i++)
	{
		auto t1 = std::chrono::system_clock::now();
		auto image = imread(files[i]);
		if (image.empty())
		{
			cerr << "cannot load " << files[i] << endl;
			abort();
		}
		auto t2 = std::chrono::system_clock::now();
		auto value_t1 = std::chrono::duration_cast<std::chrono::microseconds>(t2-t1);
		imread_time += value_t1.count();

		//# Pre-process
		if (en_hwpre)
			preprocess(prehandle, (unsigned char *)image.data, image.rows, image.cols, height, width, dpu_input_phy_addr);
		else
			preProcessSsdmobilenet(image, ip_scales[0], width, height, channels, input_ptr);

		//# Enable host to device data transfer in case software pre process
		if (!en_hwpre)	{
			for (auto &input : inTBuffs_)	{
				input->sync_for_write(0, input->get_tensor()->get_data_size() /
										input->get_tensor()->get_shape()[0]);
			}
		}
		
		//# Execution on FPGA
		auto ret = (runner)->execute_async(inTBuffs_, outTBuffs_);
		(runner)->wait(uint32_t(ret.first), -1);
		
		//# Enable device to host data transfer in case software post process
		if (!en_hwpost)	{
			for (auto output : outTBuffs_)	{
				output->sync_for_read(0, output->get_tensor()->get_data_size() /
										output->get_tensor()->get_shape()[0]);
			}
		}
		int8_t *loc_ptr = (int8_t *)outTBuffs_[0]->data().first;
		int8_t *conf = (int8_t *)outTBuffs_[1]->data().first;
		auto results = processor_[0]->ssd_post_process(conf, loc_ptr, en_hwpost);
		
		if (!profile)
			std::cout << "Detection Output of " << files[i] << " :\n";
		
		//# Print results
		for (auto &bbox : results[0].bboxes)
		{
			int label = bbox.label;
			float xmin = bbox.x * image.cols;
			float ymin = bbox.y * image.rows;
			float xmax = xmin + bbox.width * image.cols;
			float ymax = ymin + bbox.height * image.rows;
			float confidence = bbox.score;
			if (xmax > image.cols)
				xmax = image.cols;
			if (ymax > image.rows)
				ymax = image.rows;
			
			if (!profile) {
				std::cout << "label, xmin, ymin, xmax, ymax, confidence : ";
				std::cout << label << "\t" << xmin << "\t" << ymin
					  << "\t" << xmax << "\t" << ymax << "\t" << confidence << "\n";
			}
		}
	}
	auto end = std::chrono::system_clock::now();
	auto value_t1 = std::chrono::duration_cast<std::chrono::microseconds>(end-start);
	long e2e_time = value_t1.count();

	if (profile)
		std::cout << "E2E Performance: " << 1000000.0 / ((float)((e2e_time-imread_time)/count)) << " fps\n";

	if (en_hwpost)
		postproc_dealloc(posthandle);
	if (en_hwpre)
		pp_dealloc(prehandle);

	return;
}

/* 
 * Usage: 
 * app.exe <options>
 */
int main(int argc, char **argv)
{
	if (argc != 7)
	{
		std::cerr << "invalid options <exe> <config proto> <xmodel> <image dir> <pre sw/hw switch> <post sw/hw switch> <profile>\n";
		return -1;
	}
	std::string config_proto = argv[1];
	std::string xmodel_filename = argv[2];
	std::string img_dir = argv[3];
	bool en_hwpre = atoi(argv[4]);
	bool en_hwpost = atoi(argv[5]);
	bool profile = atoi(argv[6]);

	// runner
	std::unique_ptr<xir::Graph> graph = xir::Graph::deserialize(xmodel_filename);
	auto subgraph = get_dpu_subgraph(graph.get());
	auto r = vart::Runner::create_runner(subgraph[0], "run");
	auto runner_ = std::move(r.get());

	runSSD(runner_, config_proto, img_dir, profile, en_hwpost, en_hwpre);

	return 0;
}
