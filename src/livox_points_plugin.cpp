//
// Created by lfc on 2021/2/28.
//

#include "livox_laser_simulation/livox_points_plugin.h"
#include <sensor_msgs/msg/point_cloud.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud_conversion.hpp>
#include <gazebo/physics/Model.hh>
#include <gazebo/physics/MultiRayShape.hh>
#include <gazebo/physics/PhysicsEngine.hh>
#include <gazebo/physics/World.hh>
#include <gazebo/sensors/RaySensor.hh>
#include <gazebo/transport/Node.hh>
#include <gazebo_ros/node.hpp>
#include <gazebo_ros/utils.hpp>
#include <gazebo_ros/conversions/sensor_msgs.hpp>
#include <tf2_ros/transform_broadcaster.h>
#include <ament_index_cpp/get_package_prefix.hpp>
#include <ament_index_cpp/get_package_share_directory.hpp>
#include "livox_laser_simulation/csv_reader.hpp"
#include "livox_laser_simulation/livox_ode_multiray_shape.h"

namespace gazebo {

GZ_REGISTER_SENSOR_PLUGIN(LivoxPointsPlugin)

class LivoxPointsPluginPrivate
{
public:
    gazebo_ros::Node::SharedPtr _ros_node;

    void PublishPointCloud2();
    std::string _frame_name;
    rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr _pub;
    std::unique_ptr<tf2_ros::TransformBroadcaster> _tf_bro;

};

LivoxPointsPlugin::LivoxPointsPlugin()
    : _impl(std::make_unique<LivoxPointsPluginPrivate>())
{}

LivoxPointsPlugin::~LivoxPointsPlugin()
{
}

void convertDataToRotateInfo(const std::vector<std::vector<double>> &datas, std::vector<AviaRotateInfo> &avia_infos) {
    avia_infos.reserve(datas.size());
    double deg_2_rad = M_PI / 180.0;
    for (auto &data : datas) {
        if (data.size() == 3) {
            avia_infos.emplace_back();
            avia_infos.back().time = data[0];
            avia_infos.back().azimuth = data[1] * deg_2_rad;
            avia_infos.back().zenith = data[2] * deg_2_rad - M_PI_2;  //转化成标准的右手系角度
        } else {
            //ROS_INFO_STREAM("data size is not 3!");
        }
    }
}

std::string retrieveName(const std::string& url)
{
  std::string mod_url = url;
  if (url.find("package://") == 0) {
    mod_url.erase(0, strlen("package://"));
    size_t pos = mod_url.find("/");
    if (pos == std::string::npos) {
      throw std::runtime_error("Could not parse package:// format into file:// format");
    }

    std::string package = mod_url.substr(0, pos);
    if (package.empty()) {
      throw std::runtime_error("Package name must not be empty");
    }
    mod_url.erase(0, pos);
    std::string package_path;
    try {
      package_path = ament_index_cpp::get_package_share_directory(package);
    } catch (const ament_index_cpp::PackageNotFoundError &) {
      throw std::runtime_error(("Package [" + package + "] does not exist"));
    }

    //mod_url = "file://" + package_path + mod_url;
    mod_url = package_path + mod_url;
  }

  return mod_url;
}

void LivoxPointsPlugin::Load(gazebo::sensors::SensorPtr _parent, sdf::ElementPtr sdf) {
    // ROS2 node
    _impl->_ros_node = gazebo_ros::Node::Get(sdf);
    _impl->_tf_bro = std::make_unique<tf2_ros::TransformBroadcaster>(_impl->_ros_node);

    std::vector<std::vector<double>> datas;
    std::string file_name = sdf->Get<std::string>("csv_file_name");
    RCLCPP_INFO_STREAM(_impl->_ros_node->get_logger(), "load csv file name:" << file_name);
    std::cout << "load csv file name:" << file_name << std::endl;
    file_name = retrieveName(file_name);
    if (!CsvReader::ReadCsvFile(file_name, datas)) {
        RCLCPP_INFO_STREAM(_impl->_ros_node->get_logger(),
                "cannot get csv file!" << file_name << "will return !");
        return;
    }
    sdfPtr = sdf;
    auto rayElem = sdfPtr->GetElement("ray");
    auto scanElem = rayElem->GetElement("scan");
    auto rangeElem = rayElem->GetElement("range");

    int argc = 0;
    char **argv = nullptr;
    auto curr_scan_topic = sdf->Get<std::string>("ros_topic");
    RCLCPP_INFO_STREAM(_impl->_ros_node->get_logger(), "ros topic name:" << curr_scan_topic);

    // ROS2 publisher
    const gazebo_ros::QoS & qos = _impl->_ros_node->get_qos();
    rclcpp::QoS pub_qos = qos.get_publisher_qos(curr_scan_topic, rclcpp::SensorDataQoS().reliable());
    _impl->_frame_name = gazebo_ros::SensorFrameID(*_parent, *sdf);
    _impl->_pub = _impl->_ros_node->create_publisher<sensor_msgs::msg::PointCloud2>(curr_scan_topic, pub_qos);
 

    raySensor = _parent;
    auto sensor_pose = raySensor->Pose();
    SendRosTf(sensor_pose, raySensor->ParentName(), raySensor->Name());

    node = transport::NodePtr(new transport::Node());
    node->Init(raySensor->WorldName());
    scanPub = node->Advertise<msgs::LaserScanStamped>(_parent->Topic(), 50);
    aviaInfos.clear();
    convertDataToRotateInfo(datas, aviaInfos);
    RCLCPP_INFO_STREAM(_impl->_ros_node->get_logger(), "scan info size:" << aviaInfos.size());
    maxPointSize = aviaInfos.size();

    RayPlugin::Load(_parent, sdfPtr);
    laserMsg.mutable_scan()->set_frame(_parent->ParentName());
    // parentEntity = world->GetEntity(_parent->ParentName());
    parentEntity = this->world->EntityByName(_parent->ParentName());
    auto physics = world->Physics();
    laserCollision = physics->CreateCollision("multiray", _parent->ParentName());
    laserCollision->SetName("ray_sensor_collision");
    laserCollision->SetRelativePose(_parent->Pose());
    laserCollision->SetInitialRelativePose(_parent->Pose());
    rayShape.reset(new gazebo::physics::LivoxOdeMultiRayShape(laserCollision));
    laserCollision->SetShape(rayShape);
    samplesStep = sdfPtr->Get<int>("samples");
    downSample = sdfPtr->Get<int>("downsample");
    if (downSample < 1) {
        downSample = 1;
    }
    RCLCPP_INFO_STREAM(_impl->_ros_node->get_logger(), "sample:" << samplesStep);
    RCLCPP_INFO_STREAM(_impl->_ros_node->get_logger(), "downsample:" << downSample);
    rayShape->RayShapes().reserve(samplesStep / downSample);
    rayShape->Load(sdfPtr);
    rayShape->Init();
    minDist = rangeElem->Get<double>("min");
    maxDist = rangeElem->Get<double>("max");
    auto offset = laserCollision->RelativePose();
    ignition::math::Vector3d start_point, end_point;
    for (int j = 0; j < samplesStep; j += downSample) {
        int index = j % maxPointSize;
        auto &rotate_info = aviaInfos[index];
        ignition::math::Quaterniond ray;
        ray.Euler(ignition::math::Vector3d(0.0, rotate_info.zenith, rotate_info.azimuth));
        auto axis = offset.Rot() * ray * ignition::math::Vector3d(1.0, 0.0, 0.0);
        start_point = minDist * axis + offset.Pos();
        end_point = maxDist * axis + offset.Pos();
        rayShape->AddRay(start_point, end_point);
    }
}

void LivoxPointsPlugin::OnNewLaserScans() {
    if (rayShape) {
        std::vector<std::pair<int, AviaRotateInfo>> points_pair;
        InitializeRays(points_pair, rayShape);
        rayShape->Update();

        msgs::Set(laserMsg.mutable_time(), world->SimTime());
        msgs::LaserScan *scan = laserMsg.mutable_scan();
        InitializeScan(scan);

        SendRosTf(parentEntity->WorldPose(), world->Name(), raySensor->ParentName());

        auto rayCount = RayCount();
        auto verticalRayCount = VerticalRayCount();
        auto angle_min = AngleMin().Radian();
        auto angle_incre = AngleResolution();
        auto verticle_min = VerticalAngleMin().Radian();
        auto verticle_incre = VerticalAngleResolution();

        sensor_msgs::msg::PointCloud scan_point;
        scan_point.header.stamp = _impl->_ros_node->get_clock()->now();
        scan_point.header.frame_id = raySensor->Name();
        auto &scan_points = scan_point.points;

        for (auto &pair : points_pair) {
            //int verticle_index = roundf((pair.second.zenith - verticle_min) / verticle_incre);
            //int horizon_index = roundf((pair.second.azimuth - angle_min) / angle_incre);
            //if (verticle_index < 0 || horizon_index < 0) {
            //   continue;
            //}
            //if (verticle_index < verticalRayCount && horizon_index < rayCount) {
            //   auto index = (verticalRayCount - verticle_index - 1) * rayCount + horizon_index;
                auto range = rayShape->GetRange(pair.first);
                auto intensity = rayShape->GetRetro(pair.first);
                if (range >= RangeMax()) {
                    range = 0;
                } else if (range <= RangeMin()) {
                    range = 0;
                }
                //scan->set_ranges(index, range);
                //scan->set_intensities(index, intensity);

                auto rotate_info = pair.second;
                ignition::math::Quaterniond ray;
                ray.Euler(ignition::math::Vector3d(0.0, rotate_info.zenith, rotate_info.azimuth));
                //                auto axis = rotate * ray * ignition::math::Vector3d(1.0, 0.0, 0.0);
                //                auto point = range * axis + world_pose.Pos();//转换成世界坐标系

                auto axis = ray * ignition::math::Vector3d(1.0, 0.0, 0.0);
                auto point = range * axis;
                scan_points.emplace_back();
                scan_points.back().x = point.X();
                scan_points.back().y = point.Y();
                scan_points.back().z = point.Z();
            //} else {

            //    //                ROS_INFO_STREAM("count is wrong:" << verticle_index << "," << verticalRayCount << ","
            //    //                << horizon_index
            //    //                          << "," << rayCount << "," << pair.second.zenith << "," <<
            //    //                          pair.second.azimuth);
            //}
        }
        if (scanPub && scanPub->HasConnections()) scanPub->Publish(laserMsg);
        sensor_msgs::msg::PointCloud2 rosmsg;
        sensor_msgs::convertPointCloudToPointCloud2(scan_point, rosmsg);
        rosmsg.header = scan_point.header;
        _impl->_pub->publish(rosmsg);
    }
}

void LivoxPointsPlugin::InitializeRays(std::vector<std::pair<int, AviaRotateInfo>> &points_pair,
                                       boost::shared_ptr<physics::LivoxOdeMultiRayShape> &ray_shape) {
    auto &rays = ray_shape->RayShapes();
    ignition::math::Vector3d start_point, end_point;
    ignition::math::Quaterniond ray;
    auto offset = laserCollision->RelativePose();
    int64_t end_index = currStartIndex + samplesStep;
    int ray_index = 0;
    auto ray_size = rays.size();
    points_pair.reserve(rays.size());
    for (int k = currStartIndex; k < end_index; k += downSample) {
        auto index = k % maxPointSize;
        auto &rotate_info = aviaInfos[index];
        ray.Euler(ignition::math::Vector3d(0.0, rotate_info.zenith, rotate_info.azimuth));
        auto axis = offset.Rot() * ray * ignition::math::Vector3d(1.0, 0.0, 0.0);
        start_point = minDist * axis + offset.Pos();
        end_point = maxDist * axis + offset.Pos();
        if (ray_index < ray_size) {
            rays[ray_index]->SetPoints(start_point, end_point);
            points_pair.emplace_back(ray_index, rotate_info);
        }
        ray_index++;
    }
    currStartIndex += samplesStep;
}

void LivoxPointsPlugin::InitializeScan(msgs::LaserScan *&scan) {
    // Store the latest laser scans into laserMsg
    msgs::Set(scan->mutable_world_pose(), raySensor->Pose() + parentEntity->WorldPose());
    scan->set_angle_min(AngleMin().Radian());
    scan->set_angle_max(AngleMax().Radian());
    scan->set_angle_step(AngleResolution());
    scan->set_count(RangeCount());

    scan->set_vertical_angle_min(VerticalAngleMin().Radian());
    scan->set_vertical_angle_max(VerticalAngleMax().Radian());
    scan->set_vertical_angle_step(VerticalAngleResolution());
    scan->set_vertical_count(VerticalRangeCount());

    scan->set_range_min(RangeMin());
    scan->set_range_max(RangeMax());

    scan->clear_ranges();
    scan->clear_intensities();

    unsigned int rangeCount = RangeCount();
    unsigned int verticalRangeCount = VerticalRangeCount();

    for (unsigned int j = 0; j < verticalRangeCount; ++j) {
        for (unsigned int i = 0; i < rangeCount; ++i) {
            scan->add_ranges(0);
            scan->add_intensities(0);
        }
    }
}

ignition::math::Angle LivoxPointsPlugin::AngleMin() const {
    if (rayShape)
        return rayShape->MinAngle();
    else
        return -1;
}

ignition::math::Angle LivoxPointsPlugin::AngleMax() const {
    if (rayShape) {
        return ignition::math::Angle(rayShape->MaxAngle().Radian());
    } else
        return -1;
}

double LivoxPointsPlugin::GetRangeMin() const { return RangeMin(); }

double LivoxPointsPlugin::RangeMin() const {
    if (rayShape)
        return rayShape->GetMinRange();
    else
        return -1;
}

double LivoxPointsPlugin::GetRangeMax() const { return RangeMax(); }

double LivoxPointsPlugin::RangeMax() const {
    if (rayShape)
        return rayShape->GetMaxRange();
    else
        return -1;
}

double LivoxPointsPlugin::GetAngleResolution() const { return AngleResolution(); }

double LivoxPointsPlugin::AngleResolution() const { return (AngleMax() - AngleMin()).Radian() / (RangeCount() - 1); }

double LivoxPointsPlugin::GetRangeResolution() const { return RangeResolution(); }

double LivoxPointsPlugin::RangeResolution() const {
    if (rayShape)
        return rayShape->GetResRange();
    else
        return -1;
}

int LivoxPointsPlugin::GetRayCount() const { return RayCount(); }

int LivoxPointsPlugin::RayCount() const {
    if (rayShape)
        return rayShape->GetSampleCount();
    else
        return -1;
}

int LivoxPointsPlugin::GetRangeCount() const { return RangeCount(); }

int LivoxPointsPlugin::RangeCount() const {
    if (rayShape)
        return rayShape->GetSampleCount() * rayShape->GetScanResolution();
    else
        return -1;
}

int LivoxPointsPlugin::GetVerticalRayCount() const { return VerticalRayCount(); }

int LivoxPointsPlugin::VerticalRayCount() const {
    if (rayShape)
        return rayShape->GetVerticalSampleCount();
    else
        return -1;
}

int LivoxPointsPlugin::GetVerticalRangeCount() const { return VerticalRangeCount(); }

int LivoxPointsPlugin::VerticalRangeCount() const {
    if (rayShape)
        return rayShape->GetVerticalSampleCount() * rayShape->GetVerticalScanResolution();
    else
        return -1;
}

ignition::math::Angle LivoxPointsPlugin::VerticalAngleMin() const {
    if (rayShape) {
        return ignition::math::Angle(rayShape->VerticalMinAngle().Radian());
    } else
        return -1;
}

ignition::math::Angle LivoxPointsPlugin::VerticalAngleMax() const {
    if (rayShape) {
        return ignition::math::Angle(rayShape->VerticalMaxAngle().Radian());
    } else
        return -1;
}

double LivoxPointsPlugin::GetVerticalAngleResolution() const { return VerticalAngleResolution(); }

double LivoxPointsPlugin::VerticalAngleResolution() const {
    return (VerticalAngleMax() - VerticalAngleMin()).Radian() / (VerticalRangeCount() - 1);
}
void LivoxPointsPlugin::SendRosTf(const ignition::math::Pose3d &pose, const std::string &father_frame,
                                  const std::string &child_frame) {
//    if (!tfBroadcaster) {
//        tfBroadcaster.reset(new tf::TransformBroadcaster);
//    }
    geometry_msgs::msg::TransformStamped tf;
    tf.header.frame_id = raySensor->ParentName();
    tf.child_frame_id = raySensor->Name();
    tf.header.stamp = _impl->_ros_node->get_clock()->now();
    auto rot = pose.Rot();
    auto pos = pose.Pos();
    tf.transform.rotation.w = rot.W();
    tf.transform.rotation.x = rot.X();
    tf.transform.rotation.y = rot.Y();
    tf.transform.rotation.z = rot.Z();
    tf.transform.translation.x = pos.X();
    tf.transform.translation.y = pos.Y();
    tf.transform.translation.z = pos.Z();
    _impl->_tf_bro->sendTransform(tf);
}

}
