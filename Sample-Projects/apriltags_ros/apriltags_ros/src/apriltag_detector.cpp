#include <apriltags_ros/apriltag_detector.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <boost/foreach.hpp>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PoseArray.h>
#include <apriltags_ros/AprilTagCampSite.h> //our custom msg
#include <apriltags_ros/AprilTagDetection.h>
#include <apriltags_ros/AprilTagDetectionArray.h>
#include <AprilTags/Tag16h5.h>
#include <AprilTags/Tag25h7.h>
#include <AprilTags/Tag25h9.h>
#include <AprilTags/Tag36h9.h>
#include <AprilTags/Tag36h11.h>
#include <XmlRpcException.h>

namespace apriltags_ros{

AprilTagDetector::AprilTagDetector(ros::NodeHandle& nh, ros::NodeHandle& pnh): it_(nh){
  XmlRpc::XmlRpcValue april_tag_descriptions;
  if(!pnh.getParam("tag_descriptions", april_tag_descriptions)){
    ROS_WARN("No april tags specified");
  }
  else{
    try{
      descriptions_ = parse_tag_descriptions(april_tag_descriptions);
    } catch(XmlRpc::XmlRpcException e){
      ROS_ERROR_STREAM("Error loading tag descriptions: "<<e.getMessage());
    }
  }

  if(!pnh.getParam("sensor_frame_id", sensor_frame_id_)){
    sensor_frame_id_ = "";
  }

  ros::param::get("/global_namespace",global_namespace);

 // AprilTags::TagCodes tag_codes = AprilTags::tagCodes36h11;
  // AprilTags::TagCodes tag_codes = AprilTags::tagCodes36h11;
  // tag_detector_= boost::shared_ptr<AprilTags::TagDetector>(new AprilTags::TagDetector(tag_codes));



  _parameter_updated_sub = nh.subscribe("/"+global_namespace+"/parameter_updated",10,&AprilTagDetector::paramUpdatedCb,this);
  
  if(ros::param::has("/"+global_namespace+"/parameters/flyt/apriltags_family"))
    ros::param::get("/"+global_namespace+"/parameters/flyt/apriltags_family",apriltags_family);
  else
	 ros::param::set("/"+global_namespace+"/parameters/flyt/apriltags_family",apriltags_family);
  

  AprilTags::TagCodes tag_codes = AprilTags::tagCodes36h11;
  ROS_ERROR("36h11......");
  if(apriltags_family == "16h5")
  {
    ROS_ERROR("16h5......");
    tag_codes = AprilTags::tagCodes16h5;
  }      

  tag_detector_= boost::shared_ptr<AprilTags::TagDetector>(new AprilTags::TagDetector(tag_codes));



  image_sub_ = it_.subscribeCamera("/"+global_namespace+"/flytcam/image_capture", 1, &AprilTagDetector::imageCb, this);
  image_pub_ = it_.advertise("tag_detections_image", 1);
  detections_pub_ = nh.advertise<AprilTagDetectionArray>("tag_detections", 1);
  pose_pub_ = nh.advertise<geometry_msgs::PoseArray>("tag_detections_pose", 1);

  //_latlong_sub = nh.subscribe("/"+global_namespace+"/mavros/global_position/global",10,&AprilTagDetector::latLongReceivedCb,this);
  _latlong_sub = nh.subscribe("mavros/global_position/global",1, &AprilTagDetector::gposCb, this);
  _camp_site_pub = nh.advertise<AprilTagCampSite>("camp_site_coordinates",1);
}

AprilTagDetector::~AprilTagDetector(){
  image_sub_.shutdown();
}


void AprilTagDetector::gposCb(const sensor_msgs::NavSatFix::ConstPtr &gpos_msg)
{
  _last_latitude = gpos_msg->latitude;
  _last_longitude = gpos_msg->longitude;
}


void AprilTagDetector::imageCb(const sensor_msgs::ImageConstPtr& msg,const sensor_msgs::CameraInfoConstPtr& cam_info){
  cv_bridge::CvImagePtr cv_ptr;
  try{
    cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
  }
  catch (cv_bridge::Exception& e){
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }
  cv::Mat gray;
  cv::cvtColor(cv_ptr->image, gray, CV_BGR2GRAY);
  std::vector<AprilTags::TagDetection>	detections = tag_detector_->extractTags(gray);
  ROS_DEBUG("%d tag detected", (int)detections.size());

  double fx = cam_info->K[0];
  double fy = cam_info->K[4];
  double px = cam_info->K[2];
  double py = cam_info->K[5];

  if(!sensor_frame_id_.empty())
    cv_ptr->header.frame_id = sensor_frame_id_;

  AprilTagDetectionArray tag_detection_array;
  geometry_msgs::PoseArray tag_pose_array;
  tag_pose_array.header = cv_ptr->header;

  BOOST_FOREACH(AprilTags::TagDetection detection, detections){
    std::map<int, AprilTagDescription>::const_iterator description_itr = descriptions_.find(detection.id);
    if(description_itr == descriptions_.end()){
      ROS_WARN_THROTTLE(10.0, "Found tag: %d, but no description was found for it", detection.id);
      continue;
    }
    AprilTagDescription description = description_itr->second;
    double tag_size = description.size();

    //////DRAW ID MSG ON IMAGE
    detection.draw(cv_ptr->image, description.frame_name());

    Eigen::Matrix4d transform = detection.getRelativeTransform(tag_size, fx, fy, px, py);
    Eigen::Matrix3d rot = transform.block(0,0,3,3);
    Eigen::Quaternion<double> rot_quaternion = Eigen::Quaternion<double>(rot);

    geometry_msgs::PoseStamped tag_pose;
    tag_pose.pose.position.x = transform(0,3);
    tag_pose.pose.position.y = transform(1,3);
    tag_pose.pose.position.z = transform(2,3);
    tag_pose.pose.orientation.x = rot_quaternion.x();
    tag_pose.pose.orientation.y = rot_quaternion.y();
    tag_pose.pose.orientation.z = rot_quaternion.z();
    tag_pose.pose.orientation.w = rot_quaternion.w();
    tag_pose.header = cv_ptr->header;

    AprilTagDetection tag_detection;
    tag_detection.pose = tag_pose;
    tag_detection.id = detection.id;
    tag_detection.size = tag_size;
    tag_detection_array.detections.push_back(tag_detection);
    tag_pose_array.poses.push_back(tag_pose.pose);

    tf::Stamped<tf::Transform> tag_transform;
    tf::poseStampedMsgToTF(tag_pose, tag_transform);
    tf_pub_.sendTransform(tf::StampedTransform(tag_transform, tag_transform.stamp_, tag_transform.frame_id_, description.frame_name()));
    // Create a camp site message and fill it with appropriate pararmeters
    AprilTagCampSite camp_site;
    camp_site.id = detection.id;
    camp_site.x_position = tag_pose.pose.position.x;
    camp_site.y_position = tag_pose.pose.position.y;
    camp_site.latitude = _last_latitude;
    camp_site.longitude = _last_longitude;
    _camp_site_pub.publish(camp_site);

  }
  detections_pub_.publish(tag_detection_array);
  pose_pub_.publish(tag_pose_array);
  image_pub_.publish(cv_ptr->toImageMsg());
  
}


std::map<int, AprilTagDescription> AprilTagDetector::parse_tag_descriptions(XmlRpc::XmlRpcValue& tag_descriptions){
  std::map<int, AprilTagDescription> descriptions;
  ROS_ASSERT(tag_descriptions.getType() == XmlRpc::XmlRpcValue::TypeArray);
  for (int32_t i = 0; i < tag_descriptions.size(); ++i) {
    XmlRpc::XmlRpcValue& tag_description = tag_descriptions[i];
    ROS_ASSERT(tag_description.getType() == XmlRpc::XmlRpcValue::TypeStruct);
    ROS_ASSERT(tag_description["id"].getType() == XmlRpc::XmlRpcValue::TypeInt);
    ROS_ASSERT(tag_description["size"].getType() == XmlRpc::XmlRpcValue::TypeDouble);

    int id = (int)tag_description["id"];
    double size = (double)tag_description["size"];

    std::string frame_name;
    if(tag_description.hasMember("frame_id")){
      ROS_ASSERT(tag_description["frame_id"].getType() == XmlRpc::XmlRpcValue::TypeString);
      frame_name = (std::string)tag_description["frame_id"];
    }
    else{
      std::stringstream frame_name_stream;
      frame_name_stream << "tag_" << id;
      frame_name = frame_name_stream.str();
    }
    AprilTagDescription description(id, size, frame_name);
    ROS_INFO_STREAM("Loaded tag config: "<<id<<", size: "<<size<<", frame_name: "<<frame_name);
    descriptions.insert(std::make_pair(id, description));
  }
  return descriptions;
}


void AprilTagDetector::paramUpdatedCb(const std_msgs::StringConstPtr &param_name)
{
    std::string param_value;
    ROS_INFO_STREAM("param update");
    if(param_name->data == "apriltags_family")
    {
      ros::param::get("/"+global_namespace+"/parameters/flyt/apriltags_family",param_value);
      apriltags_family = param_value;

      ROS_INFO_STREAM("param family: "<<param_value);
      AprilTags::TagCodes tag_codes = AprilTags::tagCodes36h11;
      if(apriltags_family == "16h5")
      {
        ROS_INFO_STREAM("param family: 16h5");
        tag_codes = AprilTags::tagCodes16h5;
      }
      

      tag_detector_= boost::shared_ptr<AprilTags::TagDetector>(new AprilTags::TagDetector(tag_codes));
    }

}


}
