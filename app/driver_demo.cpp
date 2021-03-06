/******************************************************************************
 * Copyright 2017-2018 Baidu Robotic Vision Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <glog/logging.h>
#include <driver/helper/shared_queue.h>
#include <driver/helper/timer.h>
#include <driver/xp_aec_table.h>
#include <driver/XP_sensor_driver.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <string>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#ifdef __linux__
#include <sys/stat.h>
#endif

using std::cout;
using std::endl;
using std::vector;
using XPDRIVER::XpSensorMultithread;
using std::chrono::steady_clock;
DEFINE_bool(auto_gain, false, "turn on auto gain");
DEFINE_string(dev_id, "", "which dev to open. Empty enables auto mode");
DEFINE_bool(headless, false, "Do not show windows");
DEFINE_bool(imu_from_image, false, "Load imu from image. Helpful for USB2.0");
DEFINE_bool(save_image_bin, false, "Do not save image bin file");
DEFINE_string(sensor_type, "XP", "XP or XP2 or XP3 or FACE or XPIRL or XPIRL2");
DEFINE_bool(spacebar_mode, false, "only save img when press space bar");
DEFINE_string(record_path, "", "path to save images. Set empty to disable saving");
DEFINE_int32(valid_radius, 360,
             "The radius in pixel to check the point coverage from the pinhole center. "
             "Suggested value: 360 for 120 deg FOV and 220 for 170 deg FOV.");
DEFINE_bool(verbose, false, "whether or not log more info");

#ifdef __ARM_NEON__
DEFINE_int32(cpu_core, 4, "bind program to run on specific core[0 ~ 7],"
             "being out-of-range indicates no binding, only valid on ARM platform");
#endif

struct V4l2BufferData {
  int counter;
  std::shared_ptr<vector<uint8_t>> img_data_ptr;
  V4l2BufferData() {
    img_data_ptr.reset(new vector<uint8_t>);
  }
};
struct ImgForSave {
  cv::Mat l;
  cv::Mat r;
  cv::Mat xyz;
  std::string name;
};
struct StereoImage {
  cv::Mat l;
  cv::Mat r;
  float ts_100us;
};

XPDRIVER::shared_queue<XPDRIVER::ImuData> imu_data_queue;
XPDRIVER::shared_queue<ImgForSave> imgs_for_saving_queue;
XPDRIVER::shared_queue<ImgForSave> IR_imgs_for_saving_queue;
XPDRIVER::shared_queue<StereoImage> stereo_image_queue;
XPDRIVER::shared_queue<StereoImage> IR_image_queue;
std::atomic<bool> run_flag;
std::atomic<bool> save_img, save_ir_img;

// we use the first imu to approx img time based on img counter
std::atomic<bool> g_auto_gain;
std::atomic<bool> g_auto_infrared;
int g_aec_index;  // signed int as the index may go to negative while calculation
int g_infrared_index;
cv::Size g_img_size;
// Ideally, we should use a mutex to protect the displayed images
cv::Mat g_img_lr_display, g_img_lr_IR_display;
bool g_has_IR;

// The unique instance of XpSensorMultithread
std::unique_ptr<XPDRIVER::XpSensorMultithread> g_xp_sensor_ptr;

bool check_file_exist(const std::string file_name) {
#ifdef __linux__
  struct stat buf;
  return (stat(file_name.c_str(), &buf) == 0);
#else
  std::cout << "Function check_file_exist not implemented under this environment!\n";
  return false;
#endif
}

bool create_directory(const std::string dir_name) {
#ifdef __linux__
  const int dir_err = mkdir(dir_name.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  if (-1 == dir_err) {
      std::cout << "Error creating directory!\n";
      return false;
  }
  return true;
#else
  std::cout << "Function create_directory not implemented under this environment!\n";
  return false;
#endif
}

// Callback functions for XpSensorMultithread
// [NOTE] These callback functions have to be light-weight as it *WILL* block XpSensorMultithread
void image_data_callback(const cv::Mat& img_l, const cv::Mat& img_r, const float ts_100us) {
  if (run_flag) {
    StereoImage stereo_img;
    stereo_img.l = img_l;
    stereo_img.r = img_r;
    stereo_img.ts_100us = ts_100us;
    stereo_image_queue.push_back(stereo_img);
  }
}

void IR_data_callback(const cv::Mat& img_l, const cv::Mat& img_r, const float ts_100us) {
  if (run_flag) {
    StereoImage IR_img;
    IR_img.l = img_l;
    IR_img.r = img_r;
    IR_img.ts_100us = ts_100us;
    IR_image_queue.push_back(IR_img);
  }
}

void imu_data_callback(const XPDRIVER::ImuData& imu_data) {
  if (run_flag) {
    imu_data_queue.push_back(imu_data);
  }
}

cv::Mat_<cv::Vec3f> g_depth_xyz_img;
cv::Mat g_disparity_img;
cv::Mat g_disparity_buf;  // for filterSpeckles
vector<cv::Mat> g_disparity_ml;

bool kill_all_shared_queues() {
  imgs_for_saving_queue.kill();
  IR_imgs_for_saving_queue.kill();
  stereo_image_queue.kill();
  IR_image_queue.kill();
  imu_data_queue.kill();
  return true;
}

bool process_infrared_pwm(char keypressed) {
  if (g_xp_sensor_ptr == nullptr) {
    return false;
  }
  using XPDRIVER::XP_SENSOR::infrared_pwm_max;
  if (g_auto_infrared && keypressed != -1) {  // -1 means no key is pressed
    switch (keypressed) {
      case '6':
        g_infrared_index = 1;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '7':
        g_infrared_index = infrared_pwm_max * 0.2;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '8':
        g_infrared_index = infrared_pwm_max * 0.6;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '9':
        g_infrared_index = infrared_pwm_max - 10;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '.':
        ++g_infrared_index;
        if (g_infrared_index >= infrared_pwm_max) g_infrared_index = infrared_pwm_max -1;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '>':
        g_infrared_index += 5;
        if (g_infrared_index >= infrared_pwm_max) g_infrared_index = infrared_pwm_max -1;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case ',':
        --g_infrared_index;
        if (g_infrared_index < 0) g_infrared_index = 0;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '<':
        g_infrared_index -= 5;
        if (g_infrared_index < 0) g_infrared_index = 0;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      default:
        break;
    }
  }
  // only XPIRL and XPIRL2 support infrared light
  if ((FLAGS_sensor_type == "XPIRL" || FLAGS_sensor_type == "XPIRL2")
       && (keypressed == 'i' || keypressed == 'I')) {
    g_auto_infrared = !g_auto_infrared;
    std::cout << "set infrared mode:" << ((g_auto_infrared == true) ? "on" : "off") << std::endl;
    // TODO(zhourenyi): add auto pwm function
    if (g_auto_infrared == false)
      g_xp_sensor_ptr->set_infrared_index(0);
    g_xp_sensor_ptr->set_auto_infrared(g_auto_infrared);
  }
  return true;
}

bool process_gain_control(char keypressed) {
  if (g_xp_sensor_ptr == nullptr) {
    return false;
  }
  using XPDRIVER::XP_SENSOR::kAEC_steps;
  if (!g_auto_gain && keypressed != -1) {  // -1 means no key is pressed
    switch (keypressed) {
      case '1':
        // The lowest brightness possible
        g_aec_index = 0;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '2':
        // 20% of max brightness
        g_aec_index = kAEC_steps * 0.2;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '3':
        // 60% of max brightness
        g_aec_index = kAEC_steps * 0.6;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '4':
        // max brightness
        g_aec_index = kAEC_steps - 1;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '+':
      case '=':
        ++g_aec_index;
        if (g_aec_index >= kAEC_steps) g_aec_index = kAEC_steps - 1;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case ']':
        g_aec_index += 5;
        if (g_aec_index >= kAEC_steps) g_aec_index = kAEC_steps - 1;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '-':
        --g_aec_index;
        if (g_aec_index < 0) g_aec_index = 0;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '[':
        g_aec_index -= 5;
        if (g_aec_index < 0) g_aec_index = 0;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      default:
        break;
    }
  }
  if (keypressed == 'a' || keypressed == 'A') {
    g_auto_gain = !g_auto_gain;
    g_xp_sensor_ptr->set_auto_gain(g_auto_gain);
  }
  return true;
}

// Threads
void thread_proc_img() {
  VLOG(1) << "========= thread_proc_img thread starts";
  // mode compatibility is done in main
  cv::Mat img_l_display, img_r_display;
  if (!FLAGS_headless) {
    img_l_display = g_img_lr_display(cv::Rect(0, 0, g_img_size.width, g_img_size.height));
    img_r_display = g_img_lr_display(cv::Rect(g_img_size.width, 0,
                                              g_img_size.width, g_img_size.height));
  }
  const bool is_color = g_xp_sensor_ptr->is_color();

  cv::Mat hist_canvas(256, 256, CV_8UC3);
  cv::Mat hist_canvas_l = hist_canvas(cv::Rect(0, 0,
                                               hist_canvas.cols,
                                               hist_canvas.rows / 2));
  cv::Mat hist_canvas_r = hist_canvas(cv::Rect(0, hist_canvas.rows / 2,
                                               hist_canvas.cols,
                                               hist_canvas.rows / 2));

  // These image Mat will be assigned properly according to the sensor type
  cv::Mat img_l_mono, img_r_mono, img_l_color, img_r_color;

  size_t frame_counter = 0;
  std::chrono::time_point<steady_clock> pre_proc_time = steady_clock::now();
  float thread_proc_img_rate = 0.f;
  while (run_flag) {
    VLOG(1) << "========= thread_proc_img loop starts";
    // check if the imgs queue is too long
    bool pop_to_back = false;
    if (stereo_image_queue.size() > 10) {
      pop_to_back = true;
      LOG(ERROR) << "stereo_image_queue too long (" << stereo_image_queue.size()
                   << "). Pop to back";
    }
    StereoImage stereo_img;
    if (pop_to_back) {
      // record and calib_verify cannot be set at the same time
      if (!stereo_image_queue.wait_and_pop_to_back(&stereo_img)) {
        break;
      }
      VLOG(1) << "stereo_image_queue.wait_and_pop_front done";
    } else {
      if (!stereo_image_queue.wait_and_pop_front(&stereo_img)) {
        break;
      }
      VLOG(1) << "stereo_image_queue.wait_and_pop_front done";
    }

    // Compute the processing rate
    if (frame_counter % 10 == 0) {
      const int ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          steady_clock::now() - pre_proc_time).count();
      pre_proc_time = steady_clock::now();
      thread_proc_img_rate = 10 * 1000  / ms;
    }

    if (!FLAGS_spacebar_mode && !FLAGS_record_path.empty()) {
      // always true
      save_img = true;
    }

    // Get the mono/color image Mats properly
    // Sanity check first
    if (is_color) {
      CHECK_EQ(stereo_img.l.type(), CV_8UC3);  // sanity check
      img_l_color = stereo_img.l;
      img_r_color = stereo_img.r;
      cv::cvtColor(img_l_color, img_l_mono, cv::COLOR_BGR2GRAY);
      cv::cvtColor(img_r_color, img_r_mono, cv::COLOR_BGR2GRAY);
    } else {
      CHECK_EQ(stereo_img.l.type(), CV_8UC1);  // sanity check
      img_l_mono = stereo_img.l;
      img_r_mono = stereo_img.r;
      cv::cvtColor(img_l_mono, img_l_color, cv::COLOR_GRAY2BGR);
      cv::cvtColor(img_r_mono, img_r_color, cv::COLOR_GRAY2BGR);
    }
    if (!FLAGS_headless) {
      img_l_color.copyTo(img_l_display);
      img_r_color.copyTo(img_r_display);
    }

    // show some debug info
    std::string debug_string;
    float img_rate = g_xp_sensor_ptr->get_image_rate();
    float imu_rate = g_xp_sensor_ptr->get_imu_rate();
    char buf[100];
    snprintf(buf, sizeof(buf),
             "img %4.1f Hz imu %5.1f Hz proc %4.1f Hz time %.2f sec",
             img_rate, imu_rate, thread_proc_img_rate, stereo_img.ts_100us * 1e-4);
    debug_string = std::string(buf);
    if (!FLAGS_headless) {
      cv::putText(img_l_display, debug_string,
                  cv::Point(15, 15), cv::FONT_HERSHEY_COMPLEX, 0.5,
                  cv::Scalar(255, 0, 255), 1);
    } else {
      // FLAGS_headless is true
      std::cout << debug_string << std::endl;
    }

    if (save_img) {
      uint64_t img_time_100us = static_cast<uint64_t>(stereo_img.ts_100us);
      std::ostringstream ss;
      ss << std::setfill('0') << std::setw(10) << img_time_100us;
      ImgForSave img_for_save;
      img_for_save.name = ss.str();
      img_for_save.l = stereo_img.l.clone();  // The channels are mono: 1, color: 3
      img_for_save.r = stereo_img.r.clone();  // The channels are mono: 1, color: 3
      imgs_for_saving_queue.push_back(img_for_save);
      save_img = false;  // reset
    }
    ++frame_counter;
    usleep(1000);  // sleep for 1ms
    VLOG(1) << "========= thread_proc_img loop ends";
  }
  VLOG(1) << "========= thread_proc_img stops";
}

// Threads
void thread_proc_ir_img() {
  VLOG(1) << "========= thread_proc_ir_img thread starts";
  // mode compatibility is done in main
  cv::Mat img_l_IR_display, img_r_IR_display;
  if (!FLAGS_headless) {
    img_l_IR_display = g_img_lr_IR_display(cv::Rect(0, 0, g_img_size.width / 2,
                                                    g_img_size.height / 2));
    img_r_IR_display = g_img_lr_IR_display(cv::Rect(g_img_size.width / 2, 0,
                                                    g_img_size.width / 2, g_img_size.height / 2));
  }

  size_t frame_counter = 0;
  std::chrono::time_point<steady_clock> pre_proc_time = steady_clock::now();
  float thread_proc_img_rate = 0.f;
  while (run_flag) {
    VLOG(1) << "========= thread_proc_img loop starts";
    // check if the ir imgs queue is too long
    bool IR_pop_to_back = false;
    if (IR_image_queue.size() > 10) {
      IR_pop_to_back = true;
      LOG(ERROR) << "IR_image_queue too long (" << IR_image_queue.size()
                 << "). Pop to back";
    }
    StereoImage IR_img;
    if (IR_pop_to_back) {
      // record and calib_verify cannot be set at the same time
      if (!IR_image_queue.wait_and_pop_to_back(&IR_img)) {
        break;
      }
      VLOG(1) << "IR_image_queue.wait_and_pop_front done";
    } else {
      if (!IR_image_queue.wait_and_pop_front(&IR_img)) {
        break;
      }
      VLOG(1) << "IR_image_queue.wait_and_pop_front done";
    }
    // Compute the processing rate
    if (frame_counter % 10 == 0) {
      const int ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          steady_clock::now() - pre_proc_time).count();
      pre_proc_time = steady_clock::now();
      thread_proc_img_rate = 10 * 1000  / ms;
    }
    if (!FLAGS_headless) {
      IR_img.l.copyTo(img_l_IR_display);
      IR_img.r.copyTo(img_r_IR_display);
    }

    if (!FLAGS_spacebar_mode && !FLAGS_record_path.empty()) {
      // always true
      save_ir_img = true;
    }

    // show some debug info
    // TODO(zhourenyi): Implement get_ir_image_rate
    // float ir_img_rate = g_xp_sensor_ptr->get_ir_image_rate();
    float ir_img_rate = 0;
    char buf[100];
    snprintf(buf, sizeof(buf),
             "img %4.1f Hz proc %4.1f Hz time %.2f sec",
             ir_img_rate, thread_proc_img_rate, IR_img.ts_100us * 1e-4);
    std::string debug_string = std::string(buf);
    if (!FLAGS_headless) {
      cv::putText(g_img_lr_IR_display, debug_string,
                  cv::Point(15, 15), cv::FONT_HERSHEY_COMPLEX, 0.5,
                  cv::Scalar(255, 0, 255), 1);
    } else {
      // FLAGS_headless is true
      std::cout << debug_string << std::endl;
    }

    if (save_ir_img) {
      ImgForSave IR_img_for_save;
      uint64_t IR_img_time_100us = static_cast<uint64_t>(IR_img.ts_100us);
      std::ostringstream ss_IR;
      ss_IR << std::setfill('0') << std::setw(10) << IR_img_time_100us;
      IR_img_for_save.name = ss_IR.str();
      IR_img_for_save.l = IR_img.l.clone();
      IR_img_for_save.r = IR_img.r.clone();
      IR_imgs_for_saving_queue.push_back(IR_img_for_save);
      save_ir_img = false;  // reset
    }
    ++frame_counter;
    usleep(1000);  // sleep for 1ms
    VLOG(1) << "========= thread_proc_ir_img loop ends";
  }
  VLOG(1) << "========= thread_proc_ir_img stops";
}

void thread_save_img() {
  save_img = false;  // reset
  if (FLAGS_record_path.empty()) {
    return;
  }

  while (run_flag) {
    ImgForSave img_for_save;
    if (!imgs_for_saving_queue.wait_and_pop_to_back(&img_for_save)) {
      break;
    }
    cv::imwrite(FLAGS_record_path + "/l/" + img_for_save.name + ".png", img_for_save.l);
    cv::imwrite(FLAGS_record_path + "/r/" + img_for_save.name + ".png", img_for_save.r);
    if (img_for_save.xyz.rows > 0) {
      // save Z val
      cv::Mat_<uint16_t> z_img(img_for_save.xyz.size());
      for (int i = 0; i < img_for_save.xyz.rows; ++i) {
        for (int j = 0; j < img_for_save.xyz.cols; ++j) {
          if (img_for_save.xyz.at<cv::Vec3f>(i, j)[2] < 1e-5) {
            z_img(i, j) = 0;
          } else {
            // convert to mm
            z_img(i, j) = img_for_save.xyz.at<cv::Vec3f>(i, j)[2] * 1000;
          }
        }
      }
      cv::imwrite(FLAGS_record_path + "/Z/" + img_for_save.name + ".png", z_img);
    }
    VLOG(1) << "========= thread_save_img loop ends";
  }
  VLOG(1) << "========= thread_save_img thread stops";
}

void thread_save_ir_img() {
  VLOG(1) << "========= thread_save_ir_img thread starts";
  save_ir_img = false;  // reset
  if (FLAGS_record_path.empty()) {
    return;
  }
  while (run_flag) {
    ImgForSave img_for_save;
    if (!IR_imgs_for_saving_queue.wait_and_pop_to_back(&img_for_save)) {
      break;
    }
    cv::imwrite(FLAGS_record_path + "/l_IR/" + img_for_save.name + ".png", img_for_save.l);
    cv::imwrite(FLAGS_record_path + "/r_IR/" + img_for_save.name + ".png", img_for_save.r);
    VLOG(1) << "========= thread_save_ir_img loop ends";
  }
  VLOG(1) << "========= thread_save_ir_img thread stops";
}

void thread_write_imu_data() {
  VLOG(1) << "========= thread_write_imu_data thread starts";
  // write imu data
  std::ofstream imu_fstream;
  if (!FLAGS_record_path.empty()) {
    imu_fstream.open((FLAGS_record_path + "/imu_data.txt").c_str(), std::iostream::trunc);
    if (imu_fstream.is_open()) {
      cout << "write to " << FLAGS_record_path + "/imu_data.txt " << endl;
    } else {
      cout << "Fail to open " << FLAGS_record_path + "/imu_data.txt " << endl;
    }
  }
  while (run_flag) {
    XPDRIVER::ImuData imu_data;
    if (!imu_data_queue.wait_and_pop_front(&imu_data)) {
      break;
    }
    if (imu_fstream.is_open()) {
      const int temperature = 999;  // a fake value
      // The imu timestamp is in 100us
      // accel is in m/s^2
      // angv is in rad/s
      imu_fstream << imu_data.time_stamp << " "
                  << imu_data.accel[0] << " "
                  << imu_data.accel[1] << " "
                  << imu_data.accel[2] << " "
                  << imu_data.ang_v[0] << " "
                  << imu_data.ang_v[1] << " "
                  << imu_data.ang_v[2] << " "
                  << temperature << endl;
    }
  }
  if (imu_fstream.is_open()) {
    imu_fstream.close();
  }
  VLOG(1) << "========= thread_write_imu_data thread ends";
}

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);

#ifdef __ARM_NEON__
  if (FLAGS_cpu_core >= 0 && FLAGS_cpu_core < 8) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(FLAGS_cpu_core, &set);

    if (0 != sched_setaffinity(getpid(), sizeof(cpu_set_t), &set))
      exit(1);
    std::cout << "RUN ON CORE [" << FLAGS_cpu_core << "]" << std::endl;
  }
#endif  // __ARM_NEON__
#ifdef __linux__  // predefined by gcc
  const char* env_display_p = std::getenv("DISPLAY");
  if (!FLAGS_headless && env_display_p == nullptr) {
    std::cout << "You are running headless OS. No window will be shown" << std::endl;
    FLAGS_headless = true;
  }
#endif

  g_has_IR = (FLAGS_sensor_type == "XPIRL2");
  if (!FLAGS_record_path.empty()) {
    // First make sure record_path exists
    if (!check_file_exist(FLAGS_record_path)) {
      create_directory(FLAGS_record_path);
      cout << "Created " << FLAGS_record_path << "\n";
    }
    // If record_path already exists, we will append time at the end of
    // the record path and hopefully it will have no collision, except the special
    // case of spacebar_mode, as we may intend to continue saving images in the same
    // record path.
    std::string imu_data_file = FLAGS_record_path + "/imu_data.txt";
    if (check_file_exist(imu_data_file) && !FLAGS_spacebar_mode) {
      std::cout << "Found existing recording files at " << FLAGS_record_path << "\n";
      std::time_t t = std::time(NULL);
      char buf[32];
      std::strftime(buf, sizeof(buf), "_%H%M%S", std::localtime(&t));
      FLAGS_record_path += std::string(buf);
      std::cout << "Rename record path to " << FLAGS_record_path << "\n";
      if (create_directory(FLAGS_record_path)) {
        cout << "Created " << FLAGS_record_path << "\n";
      }
    }
    create_directory(FLAGS_record_path + "/l");
    create_directory(FLAGS_record_path + "/r");
    if (g_has_IR) {
      create_directory(FLAGS_record_path + "/l_IR");
      create_directory(FLAGS_record_path + "/r_IR");
    }
  }

  g_auto_gain = FLAGS_auto_gain;
  run_flag = true;
  g_auto_infrared = false;
  g_aec_index = 120;
  g_infrared_index = 125;

  // TODO(mingyu): Restore the support for XP3s?
  if (FLAGS_sensor_type == "XP"
      || FLAGS_sensor_type == "XP2"
      || FLAGS_sensor_type == "XP3"
      || FLAGS_sensor_type == "XPIRL"
      || FLAGS_sensor_type == "XPIRL2"
      || FLAGS_sensor_type == "FACE") {
    g_xp_sensor_ptr.reset(new XpSensorMultithread(FLAGS_sensor_type,
                                                  g_auto_gain,
                                                  FLAGS_imu_from_image,
                                                  FLAGS_dev_id));
    if (g_xp_sensor_ptr->init(g_aec_index)) {
      VLOG(1) << "XpSensorMultithread init succeeeded!";
    } else {
      LOG(ERROR) << "XpSensorMultithread failed to init";
      return -1;
    }
    uint16_t width, height;
    // default param. Will be changed if calib yaml file is provided
    if (!(g_xp_sensor_ptr->get_sensor_resolution(&width, &height)))
      return -1;

    std::string deviceID;
    if (!(g_xp_sensor_ptr->get_sensor_deviceid(&deviceID))) {
      return -1;
    } else {
      // a interface demo to call deviceID
      // LOG(ERROR) << "deviceID in driver_demo:" << *deviceID;
    }

    g_img_size.width = width;
    g_img_size.height = height;
    // FACE is a special XP3
    if (FLAGS_sensor_type == "FACE") {
      g_img_size.height = width;
      g_img_size.width = height;
    }
  } else {
    LOG(ERROR) << "sensor_type " << FLAGS_sensor_type << " not supported";
    return -1;
  }

  // cv::namedWindow has to be used in a single place
  if (!FLAGS_headless) {
    cv::namedWindow("img_lr");
    cv::moveWindow("img_lr", 1, 1);
    g_img_lr_display.create(g_img_size.height, g_img_size.width * 2, CV_8UC3);
    if (g_has_IR) {
      cv::namedWindow("img_lr_IR");
      cv::moveWindow("img_lr_IR", 10, 10);
      g_img_lr_IR_display.create(g_img_size.height / 2, g_img_size.width, CV_8UC1);
    }
  }

  // Prepare the thread pool to handle the data from XpSensorMultithread
  vector<std::thread> thread_pool;
  thread_pool.push_back(std::thread(thread_proc_img));
  if (g_has_IR) {
    thread_pool.push_back(std::thread(thread_proc_ir_img));
  }
  thread_pool.push_back(std::thread(thread_write_imu_data));
  if (!FLAGS_record_path.empty()) {
    thread_pool.push_back(std::thread(thread_save_img));
    if (g_has_IR) {
      thread_pool.push_back(std::thread(thread_save_ir_img));
    }
  }

  // Register callback functions and let XpSensorMultithread spin
  CHECK(g_xp_sensor_ptr);
  g_xp_sensor_ptr->set_image_data_callback(image_data_callback);
  if (g_has_IR) {
    g_xp_sensor_ptr->set_IR_data_callback(IR_data_callback);
  }
  g_xp_sensor_ptr->set_imu_data_callback(imu_data_callback);
  g_xp_sensor_ptr->run();

  size_t frame_counter = 0;
  if (!FLAGS_headless) {
    while (true) {
#ifdef __ARM_NEON__
      if (frame_counter % 2 == 0) // NOLINT
#endif
      {
        imshow("img_lr", g_img_lr_display);
        if (g_has_IR) {
          imshow("img_lr_IR", g_img_lr_IR_display);
        }
      }
      ++frame_counter;
      char keypressed = cv::waitKey(20);
      if (keypressed == 27) {
        // ESC
        kill_all_shared_queues();
        run_flag = false;
        break;
      } else if (keypressed == 32 && !FLAGS_record_path.empty()) {
        // space
        save_img = true;
        save_ir_img = true;
      } else if (keypressed != -1) {
        if (!process_gain_control(keypressed)) {
          LOG(ERROR) << "cannot process gain control";
        }
        if (!process_infrared_pwm(keypressed)) {
          LOG(ERROR) << "cannot process infrared control";
        }
      }
    }
  }

  for (auto& t : thread_pool) {
    t.join();
  }
  kill_all_shared_queues();
  if (!g_xp_sensor_ptr->stop()) {
    LOG(ERROR) << "XpSensorMultithread failed to stop properly!";
  }
  cout << "finished safely" << std::endl;
  return 0;
}
