// openni_tracker.cpp

#include <ros/ros.h>
#include <ros/package.h>
#include <tf/transform_broadcaster.h>
#include <std_msgs/UInt16.h>
#include <std_msgs/UInt16MultiArray.h>
#include <kdl/frames.hpp>

#include <XnOpenNI.h>
#include <XnCodecIDs.h>
#include <XnCppWrapper.h>

using std::string;

ros::Publisher available_tracked_users_pub;
ros::Publisher default_user_pub;
ros::Subscriber user_chooser_sub;
XnUserID chosen_user;

xn::Context g_Context;
xn::DepthGenerator g_DepthGenerator;
xn::UserGenerator g_UserGenerator;

XnBool g_bNeedPose = FALSE;
XnChar g_strPose[20] = "";

void XN_CALLBACK_TYPE User_NewUser(xn::UserGenerator& generator, XnUserID nId, void* pCookie)
{
  ROS_INFO("New User %d", nId);
  if (g_bNeedPose)
  {
    g_UserGenerator.GetPoseDetectionCap().StartPoseDetection(g_strPose, nId);
  }
  else
  {
    g_UserGenerator.GetSkeletonCap().RequestCalibration(nId, TRUE);
  }
}

void XN_CALLBACK_TYPE User_LostUser(xn::UserGenerator& generator, XnUserID nId, void* pCookie)
{
  ROS_INFO_STREAM("Lost user " << nId << ".");

  std_msgs::UInt16MultiArray tracked_users;
  XnUserID users[15];
  XnUInt16 users_count = 15;
  g_UserGenerator.GetUsers(users, users_count);
  for (unsigned int user = 0; user < users_count; ++user)
  {
    tracked_users.data.push_back(users[user]);
  }
  available_tracked_users_pub.publish(tracked_users);
}

void XN_CALLBACK_TYPE UserCalibration_CalibrationStart(xn::SkeletonCapability& capability, XnUserID nId, void* pCookie)
{
  ROS_INFO("Calibration started for user %d", nId);
}

void XN_CALLBACK_TYPE UserCalibration_CalibrationEnd(xn::SkeletonCapability& capability, XnUserID nId, XnBool bSuccess, void* pCookie)
{
  if (bSuccess)
  {
    ROS_INFO("Calibration complete, start tracking user %d", nId);
    g_UserGenerator.GetSkeletonCap().StartTracking(nId);

    std_msgs::UInt16MultiArray tracked_users;
    XnUserID users[15];
    XnUInt16 users_count = 15;
    g_UserGenerator.GetUsers(users, users_count);
    for (unsigned int user = 0; user < users_count; ++user)
    {
      tracked_users.data.push_back(users[user]);
    }
    available_tracked_users_pub.publish(tracked_users);
  }
  else
  {
    ROS_INFO("Calibration failed for user %d", nId);
    if (g_bNeedPose)
    {
      g_UserGenerator.GetPoseDetectionCap().StartPoseDetection(g_strPose, nId);
    }
    else
    {
      g_UserGenerator.GetSkeletonCap().RequestCalibration(nId, TRUE);
    }
  }
}

void XN_CALLBACK_TYPE UserPose_PoseDetected(xn::PoseDetectionCapability& capability, XnChar const* strPose, XnUserID nId, void* pCookie)
{
  ROS_INFO("Pose %s detected for user %d", strPose, nId);
  g_UserGenerator.GetPoseDetectionCap().StopPoseDetection(nId);
  g_UserGenerator.GetSkeletonCap().RequestCalibration(nId, TRUE);
}

void userChooserCallback(const std_msgs::UInt16ConstPtr& new_default_user)
{
  if (g_UserGenerator.GetSkeletonCap().IsTracking(new_default_user->data))
  {
    chosen_user = new_default_user->data;

    std_msgs::UInt16 default_user;
    default_user.data = chosen_user;
    default_user_pub.publish(default_user);
    ROS_INFO_STREAM("OpenNI tracker: Default user is now user " << chosen_user << ".");
  }
  else
  {
    ROS_WARN_STREAM("OpenNI tracker: There is currently no tracked user with number "
                       << new_default_user->data << ".");
  }

};

void publishTransform(XnUserID const& user, XnSkeletonJoint const& joint, string const& frame_id,
                         string const& child_frame_id)
{
  static tf::TransformBroadcaster br;

  XnSkeletonJointPosition joint_position;
  g_UserGenerator.GetSkeletonCap().GetSkeletonJointPosition(user, joint, joint_position);
  double x = -joint_position.position.X / 1000.0;
  double y = joint_position.position.Y / 1000.0;
  double z = joint_position.position.Z / 1000.0;

  XnSkeletonJointOrientation joint_orientation;
  g_UserGenerator.GetSkeletonCap().GetSkeletonJointOrientation(user, joint, joint_orientation);

  XnFloat* m = joint_orientation.orientation.elements;
  KDL::Rotation rotation(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
  double qx, qy, qz, qw;
  rotation.GetQuaternion(qx, qy, qz, qw);

  char child_frame_no[128];
  snprintf(child_frame_no, sizeof(child_frame_no), "%s_%d", child_frame_id.c_str(), user);

  tf::Transform transform;
  transform.setOrigin(tf::Vector3(x, y, z));
  transform.setRotation(tf::Quaternion(qx, -qy, -qz, qw));

  // #4994
  tf::Transform change_frame;
  change_frame.setOrigin(tf::Vector3(0, 0, 0));
  tf::Quaternion frame_rotation;
  frame_rotation.setEulerZYX(1.5708, 0, 1.5708);
  change_frame.setRotation(frame_rotation);

  transform = change_frame * transform;

  br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), frame_id, child_frame_no));
  if (user == chosen_user)
  {
    br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), frame_id, child_frame_id.c_str()));
  }
}

void publishTransforms(const std::string& frame_id)
{
  XnUserID users[15];
  XnUInt16 users_count = 15;
  XnUserID user = 0;
  g_UserGenerator.GetUsers(users, users_count);

  for (int i = 0; i <= users_count; ++i)
  {
    user = users[i];
    if (!g_UserGenerator.GetSkeletonCap().IsTracking(user))
    {
      continue;
    }
    publishTransform(user, XN_SKEL_HEAD, frame_id, "head");
    publishTransform(user, XN_SKEL_NECK, frame_id, "neck");
    publishTransform(user, XN_SKEL_TORSO, frame_id, "torso");

//    publishTransform(user, XN_SKEL_LEFT_SHOULDER, frame_id, "left_shoulder");
//    publishTransform(user, XN_SKEL_LEFT_ELBOW, frame_id, "left_elbow");
//    publishTransform(user, XN_SKEL_LEFT_HAND, frame_id, "left_hand");
    publishTransform(user, XN_SKEL_LEFT_SHOULDER, frame_id, "right_shoulder");
    publishTransform(user, XN_SKEL_LEFT_ELBOW, frame_id, "right_elbow");
    publishTransform(user, XN_SKEL_LEFT_HAND, frame_id, "right_hand");

//    publishTransform(user, XN_SKEL_RIGHT_SHOULDER, frame_id, "right_shoulder");
//    publishTransform(user, XN_SKEL_RIGHT_ELBOW, frame_id, "right_elbow");
//    publishTransform(user, XN_SKEL_RIGHT_HAND, frame_id, "right_hand");
    publishTransform(user, XN_SKEL_RIGHT_SHOULDER, frame_id, "left_shoulder");
    publishTransform(user, XN_SKEL_RIGHT_ELBOW, frame_id, "left_elbow");
    publishTransform(user, XN_SKEL_RIGHT_HAND, frame_id, "left_hand");

//    publishTransform(user, XN_SKEL_LEFT_HIP, frame_id, "left_hip");
//    publishTransform(user, XN_SKEL_LEFT_KNEE, frame_id, "left_knee");
//    publishTransform(user, XN_SKEL_LEFT_FOOT, frame_id, "left_foot");
    publishTransform(user, XN_SKEL_LEFT_HIP, frame_id, "right_hip");
    publishTransform(user, XN_SKEL_LEFT_KNEE, frame_id, "right_knee");
    publishTransform(user, XN_SKEL_LEFT_FOOT, frame_id, "right_foot");

//    publishTransform(user, XN_SKEL_RIGHT_HIP, frame_id, "right_hip");
//    publishTransform(user, XN_SKEL_RIGHT_KNEE, frame_id, "right_knee");
//    publishTransform(user, XN_SKEL_RIGHT_FOOT, frame_id, "right_foot");
    publishTransform(user, XN_SKEL_RIGHT_HIP, frame_id, "left_hip");
    publishTransform(user, XN_SKEL_RIGHT_KNEE, frame_id, "left_knee");
    publishTransform(user, XN_SKEL_RIGHT_FOOT, frame_id, "left_foot");
  }
}

#define CHECK_RC(nRetVal, what)\
if (nRetVal != XN_STATUS_OK)\
{\
  ROS_ERROR("%s failed: %s", what, xnGetStatusString(nRetVal));\
  return nRetVal;\
}

int main(int argc, char **argv)
{
ros::init(argc, argv, "openni_tracker");
ros::NodeHandle nh, nh_private("~");
ROS_INFO_STREAM("Initialising OpenNI tracker ...");

chosen_user =  0;
available_tracked_users_pub = nh_private.advertise<std_msgs::UInt16MultiArray>("available_tracked_users", 10, true);
default_user_pub = nh_private.advertise<std_msgs::UInt16>("default_user", 10, true);
user_chooser_sub = nh_private.subscribe("user_chooser", 10, userChooserCallback);

string configFilename = ros::package::getPath("openni_tracker") + "/openni_tracker.xml";
ROS_INFO_STREAM("Setting up configuration from XML file '" << configFilename << "'");
XnStatus nRetVal = g_Context.InitFromXmlFile(configFilename.c_str());
CHECK_RC(nRetVal, "InitFromXml");

ROS_INFO_STREAM("Looking for existing depth generators ...");
nRetVal = g_Context.FindExistingNode(XN_NODE_TYPE_DEPTH, g_DepthGenerator);
CHECK_RC(nRetVal, "Find depth generator");
ROS_INFO_STREAM("nRetVal: " << xnGetStatusString(nRetVal));

ROS_INFO_STREAM("Looking for existing user generators ...");
nRetVal = g_Context.FindExistingNode(XN_NODE_TYPE_USER, g_UserGenerator);
ROS_INFO_STREAM("nRetVal: " << xnGetStatusString(nRetVal));

if (nRetVal != XN_STATUS_OK)
{
  nRetVal = g_UserGenerator.Create(g_Context);
  CHECK_RC(nRetVal, "Find user generator");
  ROS_INFO_STREAM("No existing user generators found. Created new one.");
  ROS_INFO_STREAM("nRetVal: " << xnGetStatusString(nRetVal));
}

if (!g_UserGenerator.IsCapabilitySupported(XN_CAPABILITY_SKELETON))
{
  ROS_INFO_STREAM("Supplied user generator doesn't support skeleton");
  return 1;
}

ROS_INFO_STREAM("Registering user callbacks ...");
XnCallbackHandle hUserCallbacks;
g_UserGenerator.RegisterUserCallbacks(User_NewUser, User_LostUser, NULL, hUserCallbacks);

ROS_INFO_STREAM("Registering calibration callbacks ...");
XnCallbackHandle hCalibrationCallbacks;
g_UserGenerator.GetSkeletonCap().RegisterCalibrationCallbacks(UserCalibration_CalibrationStart,
                                                              UserCalibration_CalibrationEnd, NULL,
                                                              hCalibrationCallbacks);

ROS_INFO_STREAM("Checking pose detection capability ...");
if (g_UserGenerator.GetSkeletonCap().NeedPoseForCalibration())
{
  g_bNeedPose = TRUE;
  if (!g_UserGenerator.IsCapabilitySupported(XN_CAPABILITY_POSE_DETECTION))
  {
    ROS_INFO_STREAM("Pose required, but not supported");
    return 1;
  }

  ROS_INFO_STREAM("Registering pose callbacks ...");
  XnCallbackHandle hPoseCallbacks;
  g_UserGenerator.GetPoseDetectionCap().RegisterToPoseCallbacks(UserPose_PoseDetected, NULL, NULL, hPoseCallbacks);

  ROS_INFO_STREAM("Getting calibration pose ...");
  g_UserGenerator.GetSkeletonCap().GetCalibrationPose(g_strPose);
}

ROS_INFO_STREAM("Setting skeleton profile ...");
g_UserGenerator.GetSkeletonCap().SetSkeletonProfile(XN_SKEL_PROFILE_ALL);

//g_Context.Release();

ROS_INFO_STREAM("Starting to generate everything ...");
nRetVal = g_Context.StartGeneratingAll();
CHECK_RC(nRetVal, "StartGenerating");
ROS_INFO_STREAM("nRetVal: " << xnGetStatusString(nRetVal));

ROS_INFO_STREAM("Stopping to generate everything ...");
nRetVal = g_Context.StopGeneratingAll();
ROS_INFO_STREAM("nRetVal: " << xnGetStatusString(nRetVal));

ROS_INFO_STREAM("Starting to generate everything ...");
nRetVal = g_Context.StartGeneratingAll();
ROS_INFO_STREAM("nRetVal: " << xnGetStatusString(nRetVal));

ROS_INFO_STREAM("Setting up ROS node ...");
ros::Rate r(30);
ros::NodeHandle pnh("~");
string frame_id("openni_depth_frame");
pnh.getParam("camera_frame_id", frame_id);

nRetVal = g_Context.GetGlobalErrorState();
ROS_INFO_STREAM("nRetVal: " << xnGetStatusString(nRetVal));

ROS_INFO_STREAM("And go!");

while (ros::ok())
{
  ros::spinOnce();
  nRetVal = g_Context.WaitAndUpdateAll();
  CHECK_RC(nRetVal, "WaitAndUpdateAll");
  publishTransforms(frame_id);
  r.sleep();
}

g_Context.StopGeneratingAll();
g_Context.Release();
g_Context.Shutdown();
return 0;
}
