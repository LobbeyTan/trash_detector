/*
 * ros_interface.cpp
 *
 *  Created on: July 31th, 2020
 *      Author: Hilbert Xu
 *   Institute: Jupiter Robot
 */

#include <robot_vision_openvino/vino_interactive_face/ros_interface.hpp>

// Check for xServer
#include <X11/Xlib.h>

extern "C" {
	double what_time_is_it_now() {
		struct timeval time;
		if (gettimeofday(&time,NULL)) {
			return 0;
		}
		return (double)time.tv_sec + (double)time.tv_usec * .000001;
	}
}

using namespace InferenceEngine;

ROSInterface::ROSInterface(ros::NodeHandle nh)
  : nodeHandle_(nh), imageTransport_(nodeHandle_) {
    ROS_INFO("[InteractiveFace] Node Started!");

    if (!readParameters()) {
      ros::requestShutdown();
    }

    init();
}

ROSInterface::~ROSInterface() {
  {
		boost::unique_lock<boost::shared_mutex> lockNodeStatus(mutexNodeStatus_);
		isNodeRunning_ = false;
	}
	mainThread_.join();
}

bool ROSInterface::readParameters() {
	// Load common parameters.
	nodeHandle_.param("image_view/enable_opencv", viewImage_, true);
	nodeHandle_.param("image_view/wait_key_delay", waitKeyDelay_, 3);
	nodeHandle_.param("image_view/enable_console_output", enableConsoleOutput_, false);

	// Check if Xserver is running on Linux.
  if (XOpenDisplay(NULL)) {
    // Do nothing!
    ROS_INFO("[ROSInterface] Xserver is running.");
  } else {
    ROS_INFO("[ROSInterface] Xserver is not running.");
    viewImage_ = false;
  }
  std::string modelFolder_;
  std::string ageModelName_;
  std::string faceModelName_;
  std::string headPoseModelName_;
  std::string emotionsModelName_;
  std::string facialMarkModelName_;

  nodeHandle_.param("under_control",               underControl_,             bool(false));

  nodeHandle_.param("base_detector/target_device", targetDevice_,             std::string("CPU"));
  nodeHandle_.param("base_detector/model_folder",  modelFolder_,              std::string("/default"));
  nodeHandle_.param("base_detector/no_smooth",      FLAG_no_smooth,               false);

  nodeHandle_.param("face_detection/enable",          enableFaceDetection_,      true);
  nodeHandle_.param("face_detection/model_name",      faceModelName_,            std::string("/face-detection-adas-0001.xml"));
  nodeHandle_.param("face_detection/batch_size",      faceModelBatchSize_,       16);
  nodeHandle_.param("face_detection/raw_output",      faceModelRawOutput_,       false);
  nodeHandle_.param("face_detection/async",           faceModelAsync_,           false);
  nodeHandle_.param("face_detection/bb_enlarge_coef", bb_enlarge_coef,           double(1.2));
  nodeHandle_.param("face_detection/dx_coef",         dx_coef,                   double(1));
  nodeHandle_.param("face_detection/dy_coef",         dy_coef,                   double(1));

  nodeHandle_.param("age_gender/enable",           enableAgeGender_,          false);
  nodeHandle_.param("age_gender/model_name",       ageModelName_,             std::string("/age-gender-recognition-retail-0013.xml"));
  nodeHandle_.param("age_gender/batch_size",       ageModelBatchSize_,        16);
  nodeHandle_.param("age_gender/raw_output",       ageModelRawOutput_,        false);
  nodeHandle_.param("age_gender/async",            ageModelAsync_,            false);

  nodeHandle_.param("head_pose/enable",            enableHeadPose_,           false);
  nodeHandle_.param("head_pose/model_name",        headPoseModelName_,        std::string("/head-pose-estimation-adas-0001.xml"));
  nodeHandle_.param("head_pose/batch_size",        headPoseModelBatchSize_,   16);
  nodeHandle_.param("head_pose/raw_output",        headPoseModelRawOutput_,   false);
  nodeHandle_.param("head_pose/async",             headPoseModelAsync_,       false);

  nodeHandle_.param("emotions/enable",             enableEmotions_,           false);
  nodeHandle_.param("emotions/model_name",         emotionsModelName_,        std::string("/emotions-recognition-retail-0003.xml"));
  nodeHandle_.param("emotions/batch_size",         emotionsModelBatchSize_,   16);
  nodeHandle_.param("emotions/raw_output",         emotionsModelRawOutput_,   false);
  nodeHandle_.param("emotions/async",              emotionsModelAsync_,       false);

  nodeHandle_.param("facial_landmarks/enable",     enableFacialLandmarks_,    false);
  nodeHandle_.param("facial_landmarks/model_name", facialMarkModelName_,      std::string("/facial-landmarks-35-adas-0002.xml"));
  nodeHandle_.param("facial_landmarks/batch_size", facialMarkModelBatchSize_, 16);
  nodeHandle_.param("facial_landmarks/raw_output", facialMarkModelRawOutput_, false);
  nodeHandle_.param("facial_landmarks/async",      facialMarkModelAsync_,     false);

  faceModelPath_       = modelFolder_ + (faceModelName_);
  ageModelPath_        = modelFolder_ + (ageModelName_);
  headPoseModelPath_   = modelFolder_ + (headPoseModelName_);
  emotionsModelPath_   = modelFolder_ + (emotionsModelName_);
  facialMarkModelPath_ = modelFolder_ + (facialMarkModelName_);

  if (!underControl_) {
    FLAG_start_infer = true;
    FLAG_pub_message = true;
  } else {
    FLAG_start_infer = false;
    FLAG_pub_message = false;
    ROS_INFO("[ROSInterface] Waiting for command from control node...");
  }
	return true;
}


void ROSInterface::init() {
  // Initialize publisher and subscriber
	// sub camera topic properties
	std::string cameraTopicName;
	int cameraQueueSize;
	// pub detection image topic properties
	std::string detectionImageTopicName;
  int detectionImageQueueSize;
	bool detectionImageLatch;
	// sub control topic properties
  std::string subControlTopicName;
  int subControlQueueSize;
	// pub control topic properties
  std::string pubControlTopicName;
  int pubControlQueueSize;
  bool pubControlLatch;
	// pub bounding boxes topic properties
	std::string pubfaceResultsTopicName;
  int pubfaceResultsQueueSize;
  bool pubfaceResultsLatch;

	nodeHandle_.param("subscribers/camera_reading/topic",      cameraTopicName,         std::string("/astra/rgb/image_raw"));
  nodeHandle_.param("subscribers/camera_reading/queue_size", cameraQueueSize,         1);
	nodeHandle_.param("publishers/detection_image/topic",      detectionImageTopicName, std::string("detection_image"));
  nodeHandle_.param("publishers/detection_image/queue_size", detectionImageQueueSize, 1);
  nodeHandle_.param("publishers/detection_image/latch",      detectionImageLatch,     true);
  nodeHandle_.param("subscribers/control_node/topic",        subControlTopicName,     std::string("/control_to_vision"));
  nodeHandle_.param("subscribers/control_node/queue_size",   subControlQueueSize,     1);
  nodeHandle_.param("publisher/control_node/topic",          pubControlTopicName,     std::string("/vision_to_control"));
  nodeHandle_.param("publisher/control_node/queue_size",     pubControlQueueSize,     1);
  nodeHandle_.param("publisher/control_node/latch",          pubControlLatch,         false);

	nodeHandle_.param("publisher/face_results/topic", pubfaceResultsTopicName, std::string("face_results"));
  nodeHandle_.param("publisher/face_results/queue_size", pubfaceResultsQueueSize, 1);
  nodeHandle_.param("publisher/face_results/latch", pubfaceResultsLatch, false);
  
	imageSubscriber_ = imageTransport_.subscribe(cameraTopicName, cameraQueueSize, &ROSInterface::cameraCallback, this);
	detectionImagePublisher_ =
      nodeHandle_.advertise<sensor_msgs::Image>(detectionImageTopicName, detectionImageQueueSize, detectionImageLatch);
  controlSubscriber_ = nodeHandle_.subscribe(subControlTopicName, subControlQueueSize, &ROSInterface::controlCallback, this);
  controlPublisher_ = 
      nodeHandle_.advertise<robot_control_msgs::Feedback>(pubControlTopicName, pubControlQueueSize, pubControlLatch);
	faceResultsPublisher_ = 
	    nodeHandle_.advertise<robot_vision_msgs::FaceResults>(pubfaceResultsTopicName, pubfaceResultsQueueSize, pubfaceResultsLatch);

  // ????????????????????????
  faceDetector.init(faceModelPath_, targetDevice_, faceModelBatchSize_, 
                             false, faceModelAsync_, 0.5, faceModelRawOutput_, 
                             static_cast<float>(bb_enlarge_coef), static_cast<float>(dx_coef), 
                             static_cast<float>(dy_coef),
                             true);
  ageGenderDetector.init(ageModelPath_, targetDevice_, ageModelBatchSize_,
                                       true, ageModelAsync_, ageModelRawOutput_,
                                       enableAgeGender_);
  headPoseDetector.init(headPoseModelPath_, targetDevice_, headPoseModelBatchSize_,
                                       true, headPoseModelAsync_, headPoseModelRawOutput_,
                                       enableHeadPose_);
  emotionsDetector.init(emotionsModelPath_, targetDevice_, emotionsModelBatchSize_,
                                       true, emotionsModelAsync_, emotionsModelRawOutput_,
                                       enableEmotions_);
  facialLandmarksDetector.init(facialMarkModelPath_, targetDevice_, facialMarkModelBatchSize_,
                                       true, facialMarkModelAsync_, facialMarkModelRawOutput_,
                                       enableFacialLandmarks_);

  ROS_INFO("[ROSInterface] Loading device: %s", inferenceEngine_.GetVersions("CPU"));

  // ??????????????????????????????(CPU)
  // ??????true??????????????????????????????batch size???????????????????????????????????????????????????????????????
  // ?????????????????????batch size?????????????????????????????????????????????????????????????????????????????????batch size (8 or 16)
  Load(faceDetector).into(inferenceEngine_, targetDevice_, false);
  Load(ageGenderDetector).into(inferenceEngine_, targetDevice_, true);
  Load(headPoseDetector).into(inferenceEngine_, targetDevice_, true);
  Load(emotionsDetector).into(inferenceEngine_, targetDevice_, true);
  Load(facialLandmarksDetector).into(inferenceEngine_, targetDevice_, true);

  visualizer = std::make_shared<Visualizer>(cv::Size(frameWidth_, frameHeight_));

  // start Yolo thread
	mainThread_ = std::thread(&ROSInterface::mainFunc, this);

}

void ROSInterface::controlCallback(const robot_control_msgs::Mission msg) {
	// ????????????control???????????????
	if (msg.target == "face") {
		// ???????????????human pose
		if (msg.action == "detect") {
			// ????????????????????????
			FLAG_start_infer = true;
			// ??????????????????????????????
			FLAG_pub_message = true;
			ROS_INFO("[ROSInterface] Start detecting...");
		}
		else if (msg.action == "stop_detect"){
			ROS_INFO("[ROSInterface] Stop inferring...");
			// stop --> ???????????????????????????????????????
			FLAG_start_infer = false;	
			FLAG_pub_message = false;
		}
	}
}

void ROSInterface::cameraCallback(const sensor_msgs::ImageConstPtr& msg) {
	ROS_DEBUG("[ROSInterface] USB image received");

	cv_bridge::CvImagePtr cam_image;

	// ???ros???????????????????????????cv_bridge::CvImagePtr??????
	try {
		cam_image = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
	} catch (cv_bridge::Exception& e) {
		ROS_ERROR("cv_bridge exception: %s", e.what());
		return;
	}
	// ?????????????????????????????????????????????????????????????????????????????????????????????????????????
	// ????????????????????????
	if (cam_image) {
		{
			// ???mutexImageCallback_?????????(unique_lock), ?????????????????????
			boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
			imageHeader_ = msg->header;
			camImageCopy_ = cam_image->image.clone();
		}
		{
			// ???mutexImageStatus?????????, ???????????????????????????????????????????????????????????????
			boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
			imageStatus_ = true;
		}
		// ??????????????????(????????????)
		frameWidth_ = cam_image->image.size().width;
    frameHeight_ = cam_image->image.size().height;
	}
	return;
}

void ROSInterface::showImageCV(cv::Mat image) {
	cv::imshow("Interactive Face ROS on CPU", image);
}

// ???????????????????????????????????????????????????????????????
void* ROSInterface::fetchInThread() {
	{
		// ???mutexImageCallback_????????????????????????????????????????????????
		boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
		// ?????????????????????
		MatImageWithHeader_ imageWithHeader = getMatImageWithHeader();
		// ????????????????????????????????????
		buff_[buffIndex_] = imageWithHeader.image.clone();
		headerBuff_[buffIndex_] = imageWithHeader.header;
		buffId_[buffIndex_] = actionId_;
	}
}

void* ROSInterface::displayInThread(void* ptr) {
	// ?????????????????????????????????
  ROSInterface::showImageCV(buff_[(buffIndex_ + 1) % 3]);
  int c = cv::waitKey(waitKeyDelay_);
	// ??????waitKey, ????????????demo
  if (c != -1) c = c % 256;
  if (c == 27) {
    demoDone_ = 1;
    return 0;
  } 
  return 0;
}

void* ROSInterface::estimateInThread() {
	if (FLAG_start_infer) {
    size_t id = 0;
    faceDetector.enqueue(buff_[(buffIndex_+2)%3]);
    faceDetector.submitRequest();
    faceDetector.wait();
    faceDetector.fetchResults();
    auto pre_frame_result = faceDetector.results;

    for (auto &&face : pre_frame_result) {
      if (isFaceAnalyticsEnabled) {
        auto clippedRect = face.location & cv::Rect(0, 0, frameWidth_, frameHeight_);
        cv::Mat face = buff_[(buffIndex_+2)%3](clippedRect);
        ageGenderDetector.enqueue(face);
        headPoseDetector.enqueue(face);
        emotionsDetector.enqueue(face);
        facialLandmarksDetector.enqueue(face);
      }
    }
    if (isFaceAnalyticsEnabled) {
      ageGenderDetector.submitRequest();
      headPoseDetector.submitRequest();
      emotionsDetector.submitRequest();
      facialLandmarksDetector.submitRequest();
    }
    if (isFaceAnalyticsEnabled) {
      ageGenderDetector.wait();
      headPoseDetector.wait();
      emotionsDetector.wait();
      facialLandmarksDetector.wait();
    }
    // Post processing
    std::list<Face::Ptr> prev_faces;
    faces.clear();
    // For every detected face
    for (size_t i = 0; i < pre_frame_result.size(); i++) {
      robot_vision_msgs::FaceResult rosFaceResult;
      auto& result = pre_frame_result[i];
      cv::Rect rect = result.location & cv::Rect(0, 0, frameWidth_, frameHeight_);

      Face::Ptr face;
      if (!FLAG_no_smooth) {
          face = matchFace(rect, prev_faces);
          float intensity_mean = calcMean(buff_[(buffIndex_+2)%3](rect));

          if ((face == nullptr) ||
              ((std::abs(intensity_mean - face->_intensity_mean) / face->_intensity_mean) > 0.07f)) {
              face = std::make_shared<Face>(id++, rect);
          } else {
              prev_faces.remove(face);
          }

          face->_intensity_mean = intensity_mean;
          face->_location = rect;
          rosFaceResult.xmin = face->_location.x;
          rosFaceResult.ymax = face->_location.y;
          rosFaceResult.xmax = face->_location.x + face->_location.width;
          rosFaceResult.ymax = face->_location.y + face->_location.height;

      } else {
          face = std::make_shared<Face>(id++, rect);
      }

      face->ageGenderEnable((ageGenderDetector.enabled() &&
                              i < ageGenderDetector.maxBatch));
      if (face->isAgeGenderEnabled()) {
          AgeGenderDetection::Result ageGenderResult = ageGenderDetector[i];
          face->updateGender(ageGenderResult.maleProb);
          face->updateAge(ageGenderResult.age);

          rosFaceResult.gender = face->isMale() ? "Male" : "Female";
          rosFaceResult.age    = face->getAge();
      }

      face->emotionsEnable((emotionsDetector.enabled() &&
                            i < emotionsDetector.maxBatch));
      if (face->isEmotionsEnabled()) {
          face->updateEmotions(emotionsDetector[i]);
          // TODO for emotion
          // rosFaceResult.emotion = face->getMainEmotion();
      }

      face->headPoseEnable((headPoseDetector.enabled() &&
                            i < headPoseDetector.maxBatch));
      if (face->isHeadPoseEnabled()) {
          face->updateHeadPose(headPoseDetector[i]);
          rosFaceResult.angle_r = face->getHeadPose().angle_r;
          rosFaceResult.angle_p = face->getHeadPose().angle_p;
          rosFaceResult.angle_y = face->getHeadPose().angle_y;
      }

      face->landmarksEnable((facialLandmarksDetector.enabled() &&
                              i < facialLandmarksDetector.maxBatch));
      if (face->isLandmarksEnabled()) {
          face->updateLandmarks(facialLandmarksDetector[i]);
      }
      faces.push_back(face);
      detectionMsg.results.push_back(rosFaceResult);
    }
    visualizer->draw(buff_[(buffIndex_+2)%3], faces);
    //publishInThread();
    if (enableConsoleOutput_) {
			printf("\033[2J");
			printf("\033[1;1H");
			printf("\nFPS:%.1f\n", fps_);
		}
    // ??????????????????
    publishInThread();
  }
}

void ROSInterface::mainFunc() {
  isFaceAnalyticsEnabled = ageGenderDetector.enabled() || headPoseDetector.enabled() ||
                                emotionsDetector.enabled() || facialLandmarksDetector.enabled();
  if (emotionsDetector.enabled()) {
    visualizer->enableEmotionBar(emotionsDetector.emotionsVec);
  }

  const auto wait_duration = std::chrono::milliseconds(2000);
	while (!getImageStatus()) {
		printf("Waiting for image.\n");
		if (!isNodeRunning_) {
			return;
		}
		std::this_thread::sleep_for(wait_duration);
	}

	std::thread estimate_thread;
	std::thread fetch_thread;

  srand(22222222);
  {
		// ???????????????????????????, ??????????????????????????????
		boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
		MatImageWithHeader_ imageWithHeader = getMatImageWithHeader();
		cv::Mat ros_img = imageWithHeader.image.clone();
		buff_[0] = ros_img;
    headerBuff_[0] = imageWithHeader.header;
	}
	buff_[1] = buff_[0].clone();
	buff_[2] = buff_[0].clone();
	headerBuff_[1] = headerBuff_[0];
	headerBuff_[2] = headerBuff_[0];

	int count = 0;

	// ??????????????????????????????
	if (!demoPrefix_ && viewImage_) {
		cv::namedWindow("Interactive Face ROS on CPU", cv::WINDOW_NORMAL);
		cv::moveWindow("Interactive Face ROS on CPU", 0, 0);
		cv::resizeWindow("Interactive Face ROS on CPU", 640, 480);
	}
	
	demoTime_ = what_time_is_it_now();

  while(!demoDone_) {
    // buffIndex_???(0, 1, 2)?????????
		buffIndex_ = (buffIndex_ + 1) % 3;
		// ???fetchInThread????????????????????????
		fetch_thread = std::thread(&ROSInterface::fetchInThread, this);
		// ???estimateInThread????????????????????????
		estimate_thread = std::thread(&ROSInterface::estimateInThread,this);
		// ??????fps?????????
		fps_ = 1. /(what_time_is_it_now() - demoTime_);
		demoTime_ = what_time_is_it_now();

		// ??????????????????
		if (viewImage_) {
			displayInThread(0);
		} 

		// ??????fetch_thread ??? estimate_thread??????
		fetch_thread.join();
		estimate_thread.join();
		// ??????
		++count;
		// ?????????????????????????????????demo
		if (!isNodeRunning()) {
			demoDone_ = true;
		}
  }
}

MatImageWithHeader_ ROSInterface::getMatImageWithHeader() {
	cv::Mat rosImage = camImageCopy_.clone();
	MatImageWithHeader_ imageWithHeader = {.image=rosImage, .header=imageHeader_};
	return imageWithHeader;
}

bool ROSInterface::getImageStatus(void) {
	boost::shared_lock<boost::shared_mutex> lock(mutexImageStatus_);
	return imageStatus_;
}

bool ROSInterface::isNodeRunning(void) {
	boost::shared_lock<boost::shared_mutex> lock(mutexNodeStatus_);
	return isNodeRunning_;
}
 
void* ROSInterface::publishInThread() {
  if (FLAG_pub_message && !detectionMsg.results.empty()) {
    faceResultsPublisher_.publish(detectionMsg);
    detectionMsg.results.clear();
  }
}
