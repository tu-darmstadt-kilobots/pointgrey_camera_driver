/*
This code was developed by the National Robotics Engineering Center (NREC), part of the Robotics Institute at Carnegie Mellon University.
Its development was funded by DARPA under the LS3 program and submitted for public release on June 7th, 2012.
Release was granted on August, 21st 2012 with Distribution Statement "A" (Approved for Public Release, Distribution Unlimited).

This software is released under a BSD license:

Copyright (c) 2012, Carnegie Mellon University. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
Neither the name of the Carnegie Mellon University nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/



/**
   @file nodelet.cpp
   @author Chad Rockey
   @date July 13, 2011
   @brief ROS nodelet for the Point Grey Chameleon Camera

   @attention Copyright (C) 2011
   @attention National Robotics Engineering Center
   @attention Carnegie Mellon University
*/

// ROS and associated nodelet interface and PLUGINLIB declaration header
#include "ros/ros.h"
#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>

#include "pointgrey_camera_driver/PointGreyCamera.h" // The actual standalone library for the PointGreys

#include <image_transport/image_transport.h> // ROS library that allows sending compressed images
#include <camera_info_manager/camera_info_manager.h> // ROS library that publishes CameraInfo topics
#include <sensor_msgs/CameraInfo.h> // ROS message header for CameraInfo

#include <wfov_camera_msgs/WFOVImage.h>
#include <image_exposure_msgs/ExposureSequence.h> // Message type for configuring gain and white balance.

#include <diagnostic_updater/diagnostic_updater.h> // Headers for publishing diagnostic messages.
#include <diagnostic_updater/publisher.h>

#include <boost/thread.hpp> // Needed for the nodelet to launch the reading thread.

#include <dynamic_reconfigure/server.h> // Needed for the dynamic_reconfigure gui service to run

#include <kilobots_ros_tracking_msgs/ImageExposureSequence.h> //Message type to publish an image sequence

namespace pointgrey_camera_driver
{

class KilobotCameraNodelet: public nodelet::Nodelet
{
public:
  KilobotCameraNodelet():
    exposure_time_idx_(0)
  {
    exposure_times_.push_back(0.000512);
    exposure_times_.push_back(0.001024);
    exposure_times_.push_back(0.00408);
    exposure_times_.push_back(0.016304);
    exposure_times_.push_back(0.0589263);
    exposure_times_.push_back(0.107168);
    exposure_times_.push_back(0.25);
  }

  ~KilobotCameraNodelet()
  {
    if(pubThread_)
    {
      pubThread_->interrupt();
      pubThread_->join();
    }

    try
    {
      NODELET_DEBUG("Stopping camera capture.");
      pg_.stop();
      NODELET_DEBUG("Disconnecting from camera.");
      pg_.disconnect();
    }
    catch(std::runtime_error& e)
    {
      NODELET_ERROR("%s", e.what());
    }
  }

private:
  /*!
  * \brief Function that allows reconfiguration of the camera.
  *
  * This function serves as a callback for the dynamic reconfigure service.  It simply passes the configuration object to the driver to allow the camera to reconfigure.
  * \param config  camera_library::CameraConfig object passed by reference.  Values will be changed to those the driver is currently using.
  * \param level driver_base reconfiguration level.  See driver_base/SensorLevels.h for more information.
  */
  void paramCallback(pointgrey_camera_driver::PointGreyConfig &config, uint32_t level)
  {
    config_ = config;

    try
    {
      NODELET_DEBUG("Dynamic reconfigure callback with level: %d", level);
      pg_.setNewConfiguration(config, level);

      // Store needed parameters for the metadata message
      gain_ = config.gain;
      wb_blue_ = config.white_balance_blue;
      wb_red_ = config.white_balance_red;

      // Store CameraInfo binning information
      binning_x_ = 1;
      binning_y_ = 1;
      /*
      if(config.video_mode == "640x480_mono8" || config.video_mode == "format7_mode1")
      {
        binning_x_ = 2;
        binning_y_ = 2;
      }
      else if(config.video_mode == "format7_mode2")
      {
        binning_x_ = 0;
        binning_y_ = 2;
      }
      else
      {
        binning_x_ = 0;
        binning_y_ = 0;
      }
      */

      // Store CameraInfo RegionOfInterest information
      if(config.video_mode == "format7_mode0" || config.video_mode == "format7_mode1" || config.video_mode == "format7_mode2")
      {
        roi_x_offset_ = config.format7_x_offset;
        roi_y_offset_ = config.format7_y_offset;
        roi_width_ = config.format7_roi_width;
        roi_height_ = config.format7_roi_height;
        do_rectify_ = true; // Set to true if an ROI is used.
      }
      else
      {
        // Zeros mean the full resolution was captured.
        roi_x_offset_ = 0;
        roi_y_offset_ = 0;
        roi_height_ = 0;
        roi_width_ = 0;
        do_rectify_ = false; // Set to false if the whole image is captured.
      }
    }
    catch(std::runtime_error& e)
    {
      NODELET_ERROR("Reconfigure Callback failed with error: %s", e.what());
    }
  }

  int getNextIndex(int current_index)
  {
    current_index++;

    if (current_index == exposure_times_.size())
      current_index = 0;
    else if (current_index < 0)
      current_index = exposure_times_.size() + current_index;

    return current_index;
  }

  float getNextExposureTime()
  {
    exposure_time_idx_ = getNextIndex(exposure_time_idx_);
    return exposure_times_[exposure_time_idx_];
  }

  //returns shutter time in seconds, return 0.0s if shutterCount has overflow
  float getShutterTimeFromEmbeddedInfo(uint shutter_count)
  {
    // subtract 0xC2000000 to keep only the 12 lowest bits
    int shutter_count_abs = shutter_count - 3254779904;

    if (shutter_count_abs == 4095)
      return 0.0;
    else if (shutter_count_abs <= 2048)
      return 0.016 * shutter_count_abs / 1000.0;
    else
      // slope is different for values >=2048
      return (0.04711138251 * shutter_count_abs - 64.31711138) / 1000.0;
  }

  /*!
  * \brief Connection callback to only do work when someone is listening.
  *
  * This function will connect/disconnect from the camera depending on who is using the output.
  */
  void connectCb()
  {
    NODELET_DEBUG("Connect callback!");
    boost::mutex::scoped_lock scopedLock(connect_mutex_); // Grab the mutex.  Wait until we're done initializing before letting this function through.
    // Check if we should disconnect (there are 0 subscribers to our data)
    if(it_pub_.getNumSubscribers() == 0 && pub_->getPublisher().getNumSubscribers() == 0)
    {
      NODELET_DEBUG("Disconnecting.");
      pubThread_->interrupt();
      scopedLock.unlock();
      pubThread_->join();
      scopedLock.lock();
      sub_.shutdown();

      try
      {
        NODELET_DEBUG("Stopping camera capture.");
        pg_.stop();
      }
      catch(std::runtime_error& e)
      {
        NODELET_ERROR("%s", e.what());
      }

      try
      {
        NODELET_DEBUG("Disconnecting from camera.");
        pg_.disconnect();
      }
      catch(std::runtime_error& e)
      {
        NODELET_ERROR("%s", e.what());
      }
    }
    else if(!sub_)     // We need to connect
    {
      // Start the thread to loop through and publish messages
      pubThread_.reset(new boost::thread(boost::bind(&pointgrey_camera_driver::KilobotCameraNodelet::devicePoll, this)));
    }
    else
    {
      NODELET_DEBUG("Do nothing in callback.");
    }
  }

  /*!
  * \brief Serves as a psuedo constructor for nodelets.
  *
  * This function needs to do the MINIMUM amount of work to get the nodelet running.  Nodelets should not call blocking functions here.
  */
  void onInit()
  {
    // Get nodeHandles
    ros::NodeHandle &nh = getMTNodeHandle();
    ros::NodeHandle &pnh = getMTPrivateNodeHandle();

    // Get a serial number through ros
    int serial = 0;

    XmlRpc::XmlRpcValue serial_xmlrpc;
    pnh.getParam("serial", serial_xmlrpc);
    if (serial_xmlrpc.getType() == XmlRpc::XmlRpcValue::TypeInt)
    {
      pnh.param<int>("serial", serial, 0);
    }
    else if (serial_xmlrpc.getType() == XmlRpc::XmlRpcValue::TypeString)
    {
      std::string serial_str;
      pnh.param<std::string>("serial", serial_str, "0");
      std::istringstream(serial_str) >> serial;
    }
    else
    {
      NODELET_DEBUG("Serial XMLRPC type.");
      serial = 0;
    }
    NODELET_INFO("Using camera serial %d", serial);

    pg_.setDesiredCamera((uint32_t)serial);

    // Get GigE camera parameters:
    pnh.param<int>("packet_size", packet_size_, 1400);
    pnh.param<bool>("auto_packet_size", auto_packet_size_, true);
    pnh.param<int>("packet_delay", packet_delay_, 4000);

    // Set GigE parameters:
    pg_.setGigEParameters(auto_packet_size_, packet_size_, packet_delay_);

    // Get the location of our camera config yaml
    std::string camera_info_url;
    pnh.param<std::string>("camera_info_url", camera_info_url, "");
    // Get the desired frame_id, set to 'camera' if not found
    pnh.param<std::string>("frame_id", frame_id_, "camera");

    // Do not call the connectCb function until after we are done initializing.
    boost::mutex::scoped_lock scopedLock(connect_mutex_);

    // Start up the dynamic_reconfigure service, note that this needs to stick around after this function ends
    srv_ = boost::make_shared <dynamic_reconfigure::Server<pointgrey_camera_driver::PointGreyConfig> > (pnh);
    dynamic_reconfigure::Server<pointgrey_camera_driver::PointGreyConfig>::CallbackType f =
      boost::bind(&pointgrey_camera_driver::KilobotCameraNodelet::paramCallback, this, _1, _2);
    srv_->setCallback(f);

    // Start the camera info manager and attempt to load any configurations
    std::stringstream cinfo_name;
    cinfo_name << serial;
    cinfo_.reset(new camera_info_manager::CameraInfoManager(nh, cinfo_name.str(), camera_info_url));

    // Publish topics using ImageTransport through camera_info_manager (gives cool things like compression)
    it_.reset(new image_transport::ImageTransport(nh));
    image_transport::SubscriberStatusCallback cb = boost::bind(&KilobotCameraNodelet::connectCb, this);
    it_pub_ = it_->advertiseCamera("image_raw", 5, cb, cb);

    // Set up diagnostics
    updater_.setHardwareID("pointgrey_camera " + cinfo_name.str());

    // Set up a diagnosed publisher
    double desired_freq;
    pnh.param<double>("desired_freq", desired_freq, 7.0);
    pnh.param<double>("min_freq", min_freq_, desired_freq);
    pnh.param<double>("max_freq", max_freq_, desired_freq);
    double freq_tolerance; // Tolerance before stating error on publish frequency, fractional percent of desired frequencies.
    pnh.param<double>("freq_tolerance", freq_tolerance, 0.1);
    int window_size; // Number of samples to consider in frequency
    pnh.param<int>("window_size", window_size, 100);
    double min_acceptable; // The minimum publishing delay (in seconds) before warning.  Negative values mean future dated messages.
    pnh.param<double>("min_acceptable_delay", min_acceptable, 0.0);
    double max_acceptable; // The maximum publishing delay (in seconds) before warning.
    pnh.param<double>("max_acceptable_delay", max_acceptable, 0.2);
    ros::SubscriberStatusCallback cb2 = boost::bind(&KilobotCameraNodelet::connectCb, this);
    pub_.reset(new diagnostic_updater::DiagnosedPublisher<wfov_camera_msgs::WFOVImage>(nh.advertise<wfov_camera_msgs::WFOVImage>("image", 5, cb2, cb2),
               updater_,
               diagnostic_updater::FrequencyStatusParam(&min_freq_, &max_freq_, freq_tolerance, window_size),
               diagnostic_updater::TimeStampStatusParam(min_acceptable, max_acceptable)));

    seq_pub_ = nh.advertise<kilobots_ros_tracking_msgs::ImageExposureSequence>("/image_exposure_sequence", 1);
  }

  /*!
  * \brief Function for the boost::thread to grabImages and publish them.
  *
  * This function continues until the thread is interupted.  Responsible for getting sensor_msgs::Image and publishing them.
  */
  void devicePoll()
  {
    enum State
    {
        NONE
      , ERROR
      , STOPPED
      , DISCONNECTED
      , CONNECTED
      , STARTED
    };

    State state = DISCONNECTED;
    State previous_state = NONE;

    while(!boost::this_thread::interruption_requested())   // Block until we need to stop this thread.
    {
      bool state_changed = state != previous_state;

      previous_state = state;

      switch(state)
      {
        case ERROR:
          // Generally there's no need to stop before disconnecting after an
          // error. Indeed, stop will usually fail.
#if STOP_ON_ERROR
          // Try stopping the camera
          {
            boost::mutex::scoped_lock scopedLock(connect_mutex_);
            sub_.shutdown();
          }

          try
          {
            NODELET_DEBUG("Stopping camera.");
            pg_.stop();
            NODELET_INFO("Stopped camera.");

            state = STOPPED;
          }
          catch(std::runtime_error& e)
          {
            NODELET_ERROR_COND(state_changed,
                "Failed to stop error: %s", e.what());
            ros::Duration(1.0).sleep(); // sleep for one second each time
          }

          break;
#endif
        case STOPPED:
          // Try disconnecting from the camera
          try
          {
            NODELET_DEBUG("Disconnecting from camera.");
            pg_.disconnect();
            NODELET_INFO("Disconnected from camera.");

            state = DISCONNECTED;
          }
          catch(std::runtime_error& e)
          {
            NODELET_ERROR_COND(state_changed,
                "Failed to disconnect with error: %s", e.what());
            ros::Duration(1.0).sleep(); // sleep for one second each time
          }

          break;
        case DISCONNECTED:
          // Try connecting to the camera
          try
          {
            NODELET_DEBUG("Connecting to camera.");
            pg_.connect();
            NODELET_INFO("Connected to camera.");

            // Set last configuration, forcing the reconfigure level to stop
            pg_.setNewConfiguration(config_, PointGreyCamera::LEVEL_RECONFIGURE_STOP);

            // Set the timeout for grabbing images.
            try
            {
              double timeout;
              getMTPrivateNodeHandle().param("timeout", timeout, 1.0);

              NODELET_DEBUG("Setting timeout to: %f.", timeout);
              pg_.setTimeout(timeout);
            }
            catch(std::runtime_error& e)
            {
              NODELET_ERROR("%s", e.what());
            }

            // Subscribe to gain and white balance changes
            {
              boost::mutex::scoped_lock scopedLock(connect_mutex_);
              sub_ = getMTNodeHandle().subscribe("image_exposure_sequence", 10, &pointgrey_camera_driver::KilobotCameraNodelet::gainWBCallback, this);
            }

            state = CONNECTED;
          }
          catch(std::runtime_error& e)
          {
            NODELET_ERROR_COND(state_changed,
                "Failed to connect with error: %s", e.what());
            ros::Duration(1.0).sleep(); // sleep for one second each time
          }

          break;
        case CONNECTED:
          // Try starting the camera
          try
          {
            NODELET_DEBUG("Starting camera.");
            pg_.start();
            NODELET_INFO("Started camera.");

            state = STARTED;

            exp_time_ = exposure_times_[0];
            pg_.setShutter(exp_time_);

            sleep(1);
          }
          catch(std::runtime_error& e)
          {
            NODELET_ERROR_COND(state_changed,
                "Failed to start with error: %s", e.what());
            ros::Duration(1.0).sleep(); // sleep for one second each time
          }

          break;
        case STARTED:
          try
          {
            wfov_camera_msgs::WFOVImagePtr wfov_image(new wfov_camera_msgs::WFOVImage);
            // Get the image from the camera library
            NODELET_DEBUG("Starting a new grab from camera.");

            float shutter_time = getShutterTimeFromEmbeddedInfo(pg_.getShutterUnshifted());

            pg_.grabImage(wfov_image->image, frame_id_);
            pg_.setShutter(exp_time_);

            // check if shutter count overflow occured, and estimate the shutter time if needed
            if (std::abs(shutter_time) < 1e-5)
            {
              int est_exp_time_index = getNextIndex(exposure_time_idx_ -4);

              shutter_time = exposure_times_[est_exp_time_index];
            }

            imgs_.exposure_times.push_back(shutter_time);
            imgs_.images.push_back(sensor_msgs::Image(wfov_image->image));

            exp_time_ = getNextExposureTime();

            // Set other values
            wfov_image->header.frame_id = frame_id_;

            wfov_image->gain = gain_;
            wfov_image->white_balance_blue = wb_blue_;
            wfov_image->white_balance_red = wb_red_;

            wfov_image->temperature = pg_.getCameraTemperature();

            ros::Time time = ros::Time::now();
            wfov_image->header.stamp = time;
            wfov_image->image.header.stamp = time;

            // Set the CameraInfo message
            ci_.reset(new sensor_msgs::CameraInfo(cinfo_->getCameraInfo()));
            ci_->header.stamp = wfov_image->image.header.stamp;
            ci_->header.frame_id = wfov_image->header.frame_id;
            // The height, width, distortion model, and parameters are all filled in by camera info manager.
            ci_->binning_x = binning_x_;
            ci_->binning_y = binning_y_;
            ci_->roi.x_offset = roi_x_offset_;
            ci_->roi.y_offset = roi_y_offset_;
            ci_->roi.height = roi_height_;
            ci_->roi.width = roi_width_;
            ci_->roi.do_rectify = do_rectify_;

            wfov_image->info = *ci_;

            // Publish the full message
            pub_->publish(wfov_image);

            // Publish the message using standard image transport
            //if(it_pub_.getNumSubscribers() > 0)
            {
              sensor_msgs::ImagePtr image(new sensor_msgs::Image(wfov_image->image));
              it_pub_.publish(image, ci_);
            }

            if (imgs_.images.size() == exposure_times_.size())
            {
              //if(seq_pub_.getNumSubscribers() > 0)
              //{
                seq_pub_.publish(imgs_);
             // }

              imgs_.exposure_times.clear();
              imgs_.images.clear();
            }
          }
          catch(CameraTimeoutException& e)
          {
            NODELET_WARN("%s", e.what());
          }
          catch(std::runtime_error& e)
          {
            NODELET_ERROR("%s", e.what());

            state = ERROR;
          }

          break;
        default:
          NODELET_ERROR("Unknown camera state %d!", state);
      }

      // Update diagnostics
      updater_.update();
    }
    NODELET_DEBUG("Leaving thread.");
  }

  void gainWBCallback(const image_exposure_msgs::ExposureSequence &msg)
  {
    try
    {
      NODELET_DEBUG("Gain callback:  Setting gain to %f and white balances to %u, %u", msg.gain, msg.white_balance_blue, msg.white_balance_red);
      gain_ = msg.gain;
      pg_.setGain(gain_);
      wb_blue_ = msg.white_balance_blue;
      wb_red_ = msg.white_balance_red;
      pg_.setBRWhiteBalance(false, wb_blue_, wb_red_);
    }
    catch(std::runtime_error& e)
    {
      NODELET_ERROR("gainWBCallback failed with error: %s", e.what());
    }
  }

  boost::shared_ptr<dynamic_reconfigure::Server<pointgrey_camera_driver::PointGreyConfig> > srv_; ///< Needed to initialize and keep the dynamic_reconfigure::Server in scope.
  boost::shared_ptr<image_transport::ImageTransport> it_; ///< Needed to initialize and keep the ImageTransport in scope.
  boost::shared_ptr<camera_info_manager::CameraInfoManager> cinfo_; ///< Needed to initialize and keep the CameraInfoManager in scope.
  image_transport::CameraPublisher it_pub_; ///< CameraInfoManager ROS publisher
  boost::shared_ptr<diagnostic_updater::DiagnosedPublisher<wfov_camera_msgs::WFOVImage> > pub_; ///< Diagnosed publisher, has to be a pointer because of constructor requirements
  ros::Publisher seq_pub_; ///< Image sequence ROS publisher
  ros::Subscriber sub_; ///< Subscriber for gain and white balance changes.

  boost::mutex connect_mutex_;

  diagnostic_updater::Updater updater_; ///< Handles publishing diagnostics messages.
  double min_freq_;
  double max_freq_;

  PointGreyCamera pg_; ///< Instance of the PointGreyCamera library, used to interface with the hardware.
  sensor_msgs::CameraInfoPtr ci_; ///< Camera Info message.
  std::string frame_id_; ///< Frame id for the camera messages, defaults to 'camera'
  boost::shared_ptr<boost::thread> pubThread_; ///< The thread that reads and publishes the images.

  double gain_;
  uint16_t wb_blue_;
  uint16_t wb_red_;

  // Parameters for cameraInfo
  size_t binning_x_; ///< Camera Info pixel binning along the image x axis.
  size_t binning_y_; ///< Camera Info pixel binning along the image y axis.
  size_t roi_x_offset_; ///< Camera Info ROI x offset
  size_t roi_y_offset_; ///< Camera Info ROI y offset
  size_t roi_height_; ///< Camera Info ROI height
  size_t roi_width_; ///< Camera Info ROI width
  bool do_rectify_; ///< Whether or not to rectify as if part of an image.  Set to false if whole image, and true if in ROI mode.

  // For GigE cameras:
  /// If true, GigE packet size is automatically determined, otherwise packet_size_ is used:
  bool auto_packet_size_;
  /// GigE packet size:
  int packet_size_;
  /// GigE packet delay:
  int packet_delay_;

  std::vector<float> exposure_times_; // [s]
  float current_exp_time_;
  int exposure_time_idx_;
  float exp_time_;
  kilobots_ros_tracking_msgs::ImageExposureSequence imgs_;

  /// Configuration:
  pointgrey_camera_driver::PointGreyConfig config_;
};

PLUGINLIB_DECLARE_CLASS(pointgrey_camera_driver, KilobotCameraNodelet, pointgrey_camera_driver::KilobotCameraNodelet, nodelet::Nodelet);  // Needed for Nodelet declaration
}
