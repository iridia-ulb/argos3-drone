/**
 * @file <argos3/plugins/robots/drone/hardware/drone_camera_system_default_sensor.cpp>
 *
 * @author Michael Allwright - <allsey87@gmail.com>
 * @author Weixu Zhu (Harry) - <zhuweixu_harry@126.com>
 */

#include "drone_cameras_system_default_sensor.h"

#include <argos3/core/utility/configuration/tinyxml/ticpp.h>
#include <argos3/core/utility/logging/argos_log.h>
#include <argos3/core/utility/math/vector2.h>
#include <argos3/core/utility/math/vector3.h>
#include <argos3/core/utility/math/quaternion.h>

#include <apriltag/apriltag.h>
#include <apriltag/apriltag_pose.h>
#include <apriltag/tag16h5.h>
#include <apriltag/tag25h9.h>
#include <apriltag/tag36h11.h>
#include <apriltag/common/image_u8.h>
#include <apriltag/common/zarray.h>
#include <turbojpeg.h>

#include <fcntl.h>
#include <libv4l2.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <algorithm>
#include <execution>

/* hint: the command "v4l2-ctl -d0 --list-formats-ext" lists formats for /dev/video0 */
/* for testing with a set of images: https://raffaels-blog.de/en/post/fake-webcam/ */

namespace argos {

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::Init(TConfigurationNode& t_tree) {
      /* expect four cameras */
      m_vecPhysicalInterfaces.reserve(DEFAULT_SENSOR_CONFIGURATION.size());
      try {
         CCI_DroneCamerasSystemSensor::Init(t_tree);
         TConfigurationNodeIterator itInterface("camera");
         for(itInterface = itInterface.begin(&t_tree);
             itInterface != itInterface.end();
             ++itInterface) {
            std::string strId;
            GetNodeAttribute(*itInterface, "id", strId);
            std::map<std::string, TConfiguration>::const_iterator itConfig =
               DEFAULT_SENSOR_CONFIGURATION.find(strId);
            if(itConfig == std::end(DEFAULT_SENSOR_CONFIGURATION)) {
               THROW_ARGOSEXCEPTION("No configuration found for interface with id \"" << strId << "\"")
            }
            else {
               const std::string& strSensorDataPath =
                  CRobot::GetInstance().GetSensorDataPath();
               m_vecPhysicalInterfaces.emplace_back(itConfig->first,
                                                    itConfig->second,
                                                    *itInterface,
                                                    strSensorDataPath);
            }
         }
         for(SPhysicalInterface& s_physical_interface : m_vecPhysicalInterfaces) {
            /* open the device */
            s_physical_interface.Open();
         }
      }
      catch(CARGoSException& ex) {
         THROW_ARGOSEXCEPTION_NESTED("Error initializing camera sensor", ex);
      }
   }

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::Destroy() {
      const std::string& strSensorDataPath = CRobot::GetInstance().GetSensorDataPath();
      if(!strSensorDataPath.empty()) {
         ticpp::Document cMetadata(strSensorDataPath + "/metadata.xml");
         ticpp::Declaration cDecl("1.0", "", "");
         cMetadata.InsertEndChild(cDecl);
         ticpp::Element cRoot("cameras_system_metadata");
         for(SPhysicalInterface& s_physical_interface : m_vecPhysicalInterfaces) {
            cRoot.InsertEndChild(s_physical_interface.GetMetadata());
         }
         cMetadata.InsertEndChild(cRoot);
         cMetadata.SaveFile();
      }
      for(SPhysicalInterface& s_physical_interface : m_vecPhysicalInterfaces) {
            s_physical_interface.Close();
      }
   }

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::Update() {
      /* update the cameras in parallel */
      std::for_each(std::execution::par,
                    std::begin(m_vecPhysicalInterfaces),
                    std::end(m_vecPhysicalInterfaces),
                    [] (SPhysicalInterface& s_physical_interface) {
         s_physical_interface.Update();
      });
   }

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::Visit(std::function<void(SInterface&)> fn_visitor) {
      for(SPhysicalInterface& s_interface : m_vecPhysicalInterfaces) {
         fn_visitor(s_interface);
      }
   }

   /****************************************/
   /****************************************/

   CDroneCamerasSystemDefaultSensor::SPhysicalInterface::
      SPhysicalInterface(const std::string& str_label,
                         const TConfiguration& t_configuration,
                         TConfigurationNode& t_interface,
                         const std::string& str_save_path):
      SInterface(str_label),
      m_tConfiguration(t_configuration),
      m_fTagSideLength(DEFAULT_TAG_SIDE_LENGTH),
      m_strSavePath(str_save_path),
      m_cMetadata("camera") {
      /* parse camera parameters */
      GetNodeAttributeOrDefault(t_interface, "camera_brightness", m_unCameraBrightness, m_unCameraBrightness);
      GetNodeAttributeOrDefault(t_interface, "camera_contrast", m_unCameraContrast, m_unCameraContrast);
      GetNodeAttributeOrDefault(t_interface, "camera_exposure_auto_mode", m_bCameraExposureAuto, m_bCameraExposureAuto);
      GetNodeAttributeOrDefault(t_interface, "camera_exposure_absolute_time", m_unCameraExposureAbsoluteTime, m_unCameraExposureAbsoluteTime);
      /* parse calibration data if provided */
      std::string strCalibrationFilePath;
      CVector2 cFocalLength = CVector2(DEFAULT_CAMERA_PRINCIPAL_POINT_X, DEFAULT_CAMERA_PRINCIPAL_POINT_Y);
      CVector2 cPrincipalPoint = CVector2(DEFAULT_CAMERA_FOCAL_LENGTH_X, DEFAULT_CAMERA_FOCAL_LENGTH_Y);
      CVector3 cDistortionK = CVector3(0,0,0);
      CVector2 cDistortionP = CVector2(0,0);
      CVector3 cCameraPosition = std::get<CVector3>(m_tConfiguration);
      CQuaternion cCameraOrientation = std::get<CQuaternion>(m_tConfiguration);
      GetNodeAttributeOrDefault(t_interface, "calibration", strCalibrationFilePath, strCalibrationFilePath);
      if(strCalibrationFilePath.empty()) {
         LOGERR << "[WARNING] No calibration data provided for the drone camera system" << std::endl;
      }
      else {
         ticpp::Document tDocument = ticpp::Document(strCalibrationFilePath);
         tDocument.LoadFile();
         TConfigurationNode& tCalibrationNode = *tDocument.FirstChildElement();
         TConfigurationNode& tSensorNode = GetNode(tCalibrationNode, "drone_camera");
         /* read the parameters */
         GetNodeAttributeOrDefault(tSensorNode, "focal_length", cFocalLength, cFocalLength);
         GetNodeAttributeOrDefault(tSensorNode, "principal_point", cPrincipalPoint, cPrincipalPoint);
         GetNodeAttributeOrDefault(tSensorNode, "distortion_k", cDistortionK, cDistortionK);
         GetNodeAttributeOrDefault(tSensorNode, "distortion_p", cDistortionP, cDistortionP);
         GetNodeAttributeOrDefault(tSensorNode, "position", cCameraPosition, cCameraPosition);
         GetNodeAttributeOrDefault(tSensorNode, "orientation", cCameraOrientation, cCameraOrientation);
      }
      CSquareMatrix<3>& cCameraMatrix = m_sCalibration.CameraMatrix;
      cCameraMatrix.SetIdentityMatrix();
      cCameraMatrix(0,0) = cFocalLength.GetX();
      cCameraMatrix(1,1) = cFocalLength.GetY();
      cCameraMatrix(0,2) = cPrincipalPoint.GetX();
      cCameraMatrix(1,2) = cPrincipalPoint.GetY();
      m_sCalibration.DistortionK = cDistortionK;
      m_sCalibration.DistortionP = cDistortionP;
      std::get<CVector3>(m_tConfiguration) = cCameraPosition;
      std::get<CQuaternion>(m_tConfiguration) = cCameraOrientation;
      /* initialize the apriltag components */
      GetNodeAttributeOrDefault(t_interface, "tag_family", m_strTagFamilyName, m_strTagFamilyName);
      if (m_strTagFamilyName == "tag16h5")
         m_ptTagFamily = ::tag16h5_create();
      else if (m_strTagFamilyName == "tag25h9")
         m_ptTagFamily = ::tag25h9_create();
      else if (m_strTagFamilyName == "tag36h11")
         m_ptTagFamily = ::tag36h11_create();
      else
      {
         LOGERR << "[WARNING] Tag family not specified (using tag36h11 by default)." << std::endl;
         m_strTagFamilyName = "tag36h11";
         m_ptTagFamily = ::tag36h11_create();
      }
      /* create the tag detector */
      m_ptTagDetector = ::apriltag_detector_create();
      /* add the tag family to the tag detector */
      ::apriltag_detector_add_family(m_ptTagDetector, m_ptTagFamily);
      /* configure the tag detector */
      m_ptTagDetector->quad_decimate = 1.0f;
      m_ptTagDetector->quad_sigma = 0.0f;
      m_ptTagDetector->refine_edges = 1;
      m_ptTagDetector->decode_sharpening = 0.25;
      m_ptTagDetector->nthreads = 1;
      /* initialize the jpeg decoder */
      m_ptTurboJpegInstance = ::tjInitDecompress();
      /* parse the video device */
      GetNodeAttribute(t_interface, "device", m_strDevice);
      std::string strCaptureResolution;
      std::string strProcessingResolution;
      std::string strProcessingOffset;
      GetNodeAttribute(t_interface, "capture_resolution", strCaptureResolution);
      GetNodeAttribute(t_interface, "processing_resolution", strProcessingResolution);
      GetNodeAttribute(t_interface, "processing_offset", strProcessingOffset);
      ParseValues<UInt32>(strCaptureResolution, 2, m_arrCaptureResolution.data(), ',');
      ParseValues<UInt32>(strProcessingResolution, 2, m_arrProcessingResolution.data(), ',');
      ParseValues<UInt32>(strProcessingOffset, 2, m_arrProcessingOffset.data(), ',');
      LOG << "[INFO] Added camera sensor: capture resolution = "
          << static_cast<int>(m_arrCaptureResolution[0])
          << "x"
          << static_cast<int>(m_arrCaptureResolution[1])
          << ", processing resolution = "
          << static_cast<int>(m_arrProcessingResolution[0])
          << "x"
          << static_cast<int>(m_arrProcessingResolution[1])
          << " [+"
          << static_cast<int>(m_arrProcessingOffset[0])
          << ",+"
          << static_cast<int>(m_arrProcessingOffset[1])
          << "]"
          << std::endl;
      /* allocate image memory */
      if((m_arrProcessingResolution[0] + m_arrProcessingOffset[0] > m_arrCaptureResolution[0]) ||
         (m_arrProcessingResolution[1] + m_arrProcessingOffset[1] > m_arrCaptureResolution[1]))
         THROW_ARGOSEXCEPTION("Requested processing resolution is out of bounds.");
      m_ptImage =
         ::image_u8_create_alignment(m_arrCaptureResolution[0], m_arrCaptureResolution[1], 96);
      /* update the tag detection info structure */
      try {
         GetNodeAttribute(t_interface, "tag_side_length", m_fTagSideLength);
      }
      catch(CARGoSException& ex) {
         LOGERR << "[WARNING] Tag side length not specified (using default length " << m_fTagSideLength << ")." << std::endl;
      }
      m_tTagDetectionInfo.fx = m_sCalibration.CameraMatrix(0,0);
      m_tTagDetectionInfo.fy = m_sCalibration.CameraMatrix(1,1);
      m_tTagDetectionInfo.cx = m_sCalibration.CameraMatrix(0,2);
      m_tTagDetectionInfo.cy = m_sCalibration.CameraMatrix(1,2);
      m_tTagDetectionInfo.tagsize = m_fTagSideLength;
      /* set attributes on the camera metadata tag */
      m_cMetadata.SetAttribute("id", str_label);
      m_cMetadata.SetAttribute("processing_offset", strProcessingOffset);
   }

   /****************************************/
   /****************************************/

   CDroneCamerasSystemDefaultSensor::SPhysicalInterface::~SPhysicalInterface() {
      /* deallocate image memory */
      ::image_u8_destroy(m_ptImage);
      /* uninitialize the apriltag components */
      ::apriltag_detector_remove_family(m_ptTagDetector, m_ptTagFamily);
      /* destroy the tag detector */
      ::apriltag_detector_destroy(m_ptTagDetector);
      /* destroy the tag family */
      ::tag36h11_destroy(m_ptTagFamily);
      /* destroy the jpeg decoder */
      ::tjDestroy(m_ptTurboJpegInstance);
   }

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::SPhysicalInterface::Open() {
      if ((m_nCameraHandle = ::open(m_strDevice.c_str(), O_RDWR, 0)) < 0)
         THROW_ARGOSEXCEPTION("Could not open " << m_strDevice);
      int nInput = 0;
      if (::ioctl(m_nCameraHandle, VIDIOC_S_INPUT, &nInput) < 0)
         THROW_ARGOSEXCEPTION("Could not set " << m_strDevice << " as an input");
      /* set camera format*/
      v4l2_format sFormat;
      sFormat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      sFormat.fmt.pix.width = m_arrCaptureResolution[0];
      sFormat.fmt.pix.height = m_arrCaptureResolution[1];
      sFormat.fmt.pix.bytesperline = 0;
      sFormat.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
      sFormat.fmt.pix.field = V4L2_FIELD_NONE;
      if (::ioctl(m_nCameraHandle, VIDIOC_S_FMT, &sFormat) < 0)
         THROW_ARGOSEXCEPTION("Could not set the camera format");
      /* set camera control brightness */
      struct v4l2_control sBrightnessControl;
      memset(&sBrightnessControl, 0, sizeof (sBrightnessControl));
      sBrightnessControl.id = V4L2_CID_BRIGHTNESS;
      sBrightnessControl.value = m_unCameraBrightness;
      if (::ioctl(m_nCameraHandle, VIDIOC_S_CTRL, &sBrightnessControl) < 0)
         THROW_ARGOSEXCEPTION("Could not set the camera brightness");
      /* set camera control contrast */
      struct v4l2_control sContrastControl;
      memset(&sContrastControl, 0, sizeof (sContrastControl));
      sContrastControl.id = V4L2_CID_CONTRAST;
      sContrastControl.value = m_unCameraContrast;
      if (::ioctl(m_nCameraHandle, VIDIOC_S_CTRL, &sContrastControl) < 0)
         THROW_ARGOSEXCEPTION("Could not set the camera contrast");
      /* set camera control exposure mode */
      struct v4l2_control sExposureAutoModeControl;
      memset(&sExposureAutoModeControl, 0, sizeof (sExposureAutoModeControl));
      sExposureAutoModeControl.id = V4L2_CID_EXPOSURE_AUTO;
      if (m_bCameraExposureAuto)
         sExposureAutoModeControl.value = V4L2_EXPOSURE_APERTURE_PRIORITY;
      else
         sExposureAutoModeControl.value = V4L2_EXPOSURE_MANUAL;
      if (::ioctl(m_nCameraHandle, VIDIOC_S_CTRL, &sExposureAutoModeControl) < 0)
         THROW_ARGOSEXCEPTION("Could not set the camera exposure auto mode");
      /* request camera buffers */
      v4l2_requestbuffers sRequest;
      ::memset(&sRequest, 0, sizeof(sRequest));
      sRequest.count = m_arrBuffers.size();
      sRequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      sRequest.memory= V4L2_MEMORY_MMAP;
      if (::ioctl(m_nCameraHandle, VIDIOC_REQBUFS, &sRequest) < 0) {
         THROW_ARGOSEXCEPTION("Could not request buffers");
      }
      if (sRequest.count < m_arrBuffers.size()) {
         THROW_ARGOSEXCEPTION("Could not get the requested number of buffers");
      }
      /* remap and enqueue the buffers */
      v4l2_buffer sBuffer;
      UInt32 unBufferIndex = 0;
      for(std::pair<UInt32, void*>& t_buffer : m_arrBuffers) {
         /* zero the buffer */
         ::memset(&sBuffer, 0, sizeof(v4l2_buffer));
         sBuffer.type = ::V4L2_BUF_TYPE_VIDEO_CAPTURE;
         sBuffer.memory = ::V4L2_MEMORY_MMAP;
         sBuffer.index = unBufferIndex;
         if(::ioctl(m_nCameraHandle, VIDIOC_QUERYBUF, &sBuffer) < 0) {
            THROW_ARGOSEXCEPTION("Could not query buffer");
         }
         std::get<UInt32>(t_buffer) =
            unBufferIndex;
         std::get<void*>(t_buffer) =
            ::mmap(nullptr, sBuffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                   m_nCameraHandle, sBuffer.m.offset);
         /* increment the buffer index */
         unBufferIndex++;
      }
      /* intialize iterators for the buffers */
      m_itNextBuffer = std::begin(m_arrBuffers);
      m_itCurrentBuffer = std::end(m_arrBuffers);
      /* start the stream */
      enum v4l2_buf_type eBufferType = ::V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if(::ioctl(m_nCameraHandle, VIDIOC_STREAMON, &eBufferType) < 0) {
         THROW_ARGOSEXCEPTION("Could not start the stream");
      }
   }

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::SPhysicalInterface::Enable() {
      if(Enabled == false) {
         /* enqueue the first buffer */
         ::v4l2_buffer sBuffer;
         ::memset(&sBuffer, 0, sizeof(::v4l2_buffer));
         sBuffer.type = ::V4L2_BUF_TYPE_VIDEO_CAPTURE;
         sBuffer.memory = ::V4L2_MEMORY_MMAP;
         sBuffer.index = m_itNextBuffer->first;
         if(::ioctl(m_nCameraHandle, VIDIOC_QBUF, &sBuffer) < 0) {
            THROW_ARGOSEXCEPTION("Could not enqueue used buffer");
         }
         /* call base class method */
         SInterface::Enable();
      }
   }

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::SPhysicalInterface::Disable() {
      if(Enabled == true) {
         /* call base class method */
         SInterface::Disable();
         /* dequeue the next buffer */
         ::v4l2_buffer sBuffer;
         memset(&sBuffer, 0, sizeof(v4l2_buffer));
         sBuffer.type = ::V4L2_BUF_TYPE_VIDEO_CAPTURE;
         sBuffer.memory = ::V4L2_MEMORY_MMAP;
         sBuffer.index = m_itNextBuffer->first;
         if(::ioctl(m_nCameraHandle, VIDIOC_DQBUF, &sBuffer) < 0) {
            LOGERR << "[WARNING] Could not dequeue buffer" << std::endl;
         }
      }
   }

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::SPhysicalInterface::Close() {
      /* disable the sensor */
      Disable();
      /* stop the stream */
      enum v4l2_buf_type eBufferType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (::ioctl(m_nCameraHandle, VIDIOC_STREAMOFF, &eBufferType) < 0) {
         LOGERR << "[WARNING] Could not stop the stream" << std::endl;
      }
      /* close the camera */
      ::close(m_nCameraHandle);
   }

   /****************************************/
   /****************************************/

   void CDroneCamerasSystemDefaultSensor::SPhysicalInterface::Update() {
      /* clear out previous readings */
      Tags.clear();
      /* if enabled, process the next buffer */
      if(Enabled == true) {
         try {
            /* update the buffer iterators */
            m_itCurrentBuffer = m_itNextBuffer;
            m_itNextBuffer++;
            if(m_itNextBuffer == std::end(m_arrBuffers)) {
               m_itNextBuffer = std::begin(m_arrBuffers);
            }
            /* dequeue the buffer */
            ::v4l2_buffer sBuffer;
            memset(&sBuffer, 0, sizeof(v4l2_buffer));
            sBuffer.type = ::V4L2_BUF_TYPE_VIDEO_CAPTURE;
            sBuffer.memory = ::V4L2_MEMORY_MMAP;
            sBuffer.index = m_itCurrentBuffer->first;
            if(::ioctl(m_nCameraHandle, VIDIOC_DQBUF, &sBuffer) < 0) {
               THROW_ARGOSEXCEPTION("Could not dequeue buffer");
            }
            /* set exposure time after first frame */
            if ((!m_bCameraExposureAuto) && (!m_bExposureTimeSetFlag)) {
               m_bExposureTimeSetFlag = true;
               /* set camera control exposure time*/
               struct v4l2_control sExposureTimeControl;
               memset(&sExposureTimeControl, 0, sizeof (sExposureTimeControl));
               sExposureTimeControl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
               sExposureTimeControl.value = m_unCameraExposureAbsoluteTime;
               if (::ioctl(m_nCameraHandle, VIDIOC_S_CTRL, &sExposureTimeControl) < 0)
                  THROW_ARGOSEXCEPTION("Could not set camera exposure time");
            }
            /* update the timestamp in the control interface */
            Timestamp = sBuffer.timestamp.tv_sec + (10e-6 * sBuffer.timestamp.tv_usec);
            /* decompress the JPEG data directly into the Apriltag image */
            ::tjDecompress2(m_ptTurboJpegInstance, 
                            static_cast<const unsigned char*>(m_itCurrentBuffer->second),
                            sBuffer.length,
                            m_ptImage->buf,
                            m_ptImage->width,
                            m_ptImage->stride,
                            m_ptImage->height,
                            ::TJPF_GRAY,
                            TJFLAG_FASTDCT);
            /* crop ptImage to tImageProcess */
            image_u8_t tImageProcess = {
               static_cast<int32_t>(m_arrProcessingResolution[0]),
               static_cast<int32_t>(m_arrProcessingResolution[1]),
               m_ptImage->stride,
               m_ptImage->buf +
                  static_cast<size_t>(m_arrProcessingOffset[0] +
                                      m_arrProcessingOffset[1] * m_ptImage->stride),
            };
            /* detect the tags */
            CVector2 cCenterPixel;
            std::array<CVector2, 4> arrCornerPixels;
            /* run the apriltags algorithm */
            ::zarray_t* ptDetectionArray =
               ::apriltag_detector_detect(m_ptTagDetector, &tImageProcess);
            /* get the detected tags count */
            size_t unTagCount = static_cast<size_t>(::zarray_size(ptDetectionArray));
            /* reserve space for the tags */
            Tags.reserve(unTagCount);
            /* copy detection data to the control interface */
            for(size_t un_index = 0; un_index < unTagCount; un_index++) {
               ::apriltag_detection_t *ptDetection;
               ::zarray_get(ptDetectionArray, un_index, &ptDetection);
               /* undistort corner and center pixels */
               UndistortPixel(ptDetection->p[0][0], ptDetection->p[0][1]);
               UndistortPixel(ptDetection->p[1][0], ptDetection->p[1][1]);
               UndistortPixel(ptDetection->p[2][0], ptDetection->p[2][1]);
               UndistortPixel(ptDetection->p[3][0], ptDetection->p[3][1]);
               UndistortPixel(ptDetection->c[0],    ptDetection->c[1]);
               /* copy the tag corner coordinates */
               arrCornerPixels[0].Set(ptDetection->p[0][0], ptDetection->p[0][1]);
               arrCornerPixels[1].Set(ptDetection->p[1][0], ptDetection->p[1][1]);
               arrCornerPixels[2].Set(ptDetection->p[2][0], ptDetection->p[2][1]);
               arrCornerPixels[3].Set(ptDetection->p[3][0], ptDetection->p[3][1]);
               /* copy the tag center coordinate */
               cCenterPixel.Set(ptDetection->c[0], ptDetection->c[1]);
               /* calculate tag pose */
               apriltag_pose_t tPose;
               m_tTagDetectionInfo.det = ptDetection;
               ::estimate_tag_pose(&m_tTagDetectionInfo, &tPose);
               CRotationMatrix3 cTagOrientation(tPose.R->data);
               CVector3 cTagPosition(tPose.t->data[0], tPose.t->data[1], tPose.t->data[2]);
               /* copy readings */
               Tags.emplace_back(ptDetection->id, cTagPosition, cTagOrientation, cCenterPixel, arrCornerPixels);
            }
            /* destroy the readings array */
            ::apriltag_detections_destroy(ptDetectionArray);
            /* save frame data and metadata to files if a path to do so was specified */
            if(!m_strSavePath.empty()) {
               /* create a new frame node */
               ticpp::Element cFrameElement("frame");
               cFrameElement.SetAttribute("timestamp", Timestamp);
               cFrameElement.SetAttribute("index", m_unFrameIndex);
               /* write images to the save path */
               std::string strBasename = m_strSavePath +
                  "/frame" + std::to_string(m_unFrameIndex) +
                  "_" + Label;
               /* PNM image */
               ticpp::Element cPnmElement("pnm");
               cPnmElement.SetAttribute("path", strBasename + ".pnm");
               cFrameElement.InsertEndChild(cPnmElement);
               image_u8_write_pnm(&tImageProcess, (strBasename + ".pnm").c_str());
               /* JPEG image */
               ticpp::Element cJpegElement("jpeg");
               cJpegElement.SetAttribute("path", strBasename + ".jpg");
               cFrameElement.InsertEndChild(cJpegElement);
               std::ofstream(strBasename + ".jpg")
                  .write(static_cast<const char*>(m_itCurrentBuffer->second), sBuffer.length);
               /* write metadata for each detection */
               for(const STag& s_tag: Tags) {
                  ticpp::Element cDetectionElement("detection");
                  cDetectionElement.SetAttribute("id", s_tag.Id);
                  for(UInt32 un_index = 0; un_index < s_tag.Corners.size(); un_index++) {
                     cDetectionElement.SetAttribute('p' + std::to_string(un_index), s_tag.Corners[un_index]);
                  }
                  cDetectionElement.SetAttribute("c", s_tag.Center);
                  cFrameElement.InsertEndChild(cDetectionElement);
               }
               m_cMetadata.InsertEndChild(cFrameElement);
            }
            /* enqueue the next buffer */
            ::memset(&sBuffer, 0, sizeof(::v4l2_buffer));
            sBuffer.type = ::V4L2_BUF_TYPE_VIDEO_CAPTURE;
            sBuffer.memory = ::V4L2_MEMORY_MMAP;
            sBuffer.index = m_itNextBuffer->first;
            if(::ioctl(m_nCameraHandle, VIDIOC_QBUF, &sBuffer) < 0) {
               THROW_ARGOSEXCEPTION("Could not enqueue used buffer");
            }
            m_unFrameIndex += 1;
         }
         catch(CARGoSException& ex) {
            THROW_ARGOSEXCEPTION_NESTED("Error updating the camera sensor", ex);
         }
      }
   }

   /****************************************/
   /****************************************/
   
   void CDroneCamerasSystemDefaultSensor::SPhysicalInterface::UndistortPixel(double& fU, double& fV)
   {
      /* U,V are pixels in the image.
         U+ to the right <--> x,
         V+ down         <--> y  */
      /* This function operates on apriltag variables which are doubles rather than Reals */
      double fFx = m_sCalibration.CameraMatrix(0,0);
      double fFy = m_sCalibration.CameraMatrix(1,1);
      double fCx = m_sCalibration.CameraMatrix(0,2);
      double fCy = m_sCalibration.CameraMatrix(1,2);
      double fK1 = m_sCalibration.DistortionK.GetX();
      double fK2 = m_sCalibration.DistortionK.GetY();
      double fK3 = m_sCalibration.DistortionK.GetZ();
      double fP1 = m_sCalibration.DistortionP.GetX();
      double fP2 = m_sCalibration.DistortionP.GetY();
      double fXDistortion = (fU - fCx) / fFx;
      double fYDistortion = (fV - fCy) / fFy;
      double fXCorrected, fYCorrected;
      double fX0 = fXDistortion;
      double fY0 = fYDistortion;
      /* start iteration */
      for (UInt8 unI = 0; unI < UNDISTORT_ITERATIONS; unI++)
      {
         double fR2 = std::pow(fXDistortion, 2) + std::pow(fYDistortion, 2);
         double fK = 1 / (1. + fK1 * fR2 + fK2 * std::pow(fR2, 2) + fK3 * std::pow(fR2, 3));
         double fDx = 2. * fP1 * fXDistortion * fYDistortion + fP2 * (fR2 + 2. * std::pow(fXDistortion, 2));
         double fDy = fP1 * (fR2 + 2. * std::pow(fYDistortion, 2)) + 2. * fP2 * fXDistortion * fYDistortion;
         fXCorrected = (fX0 - fDx) * fK;
         fYCorrected = (fY0 - fDy) * fK;
         fXDistortion = fXCorrected;
         fYDistortion = fYCorrected;
      }
      fXCorrected = fXCorrected * fFx + fCx;
      fYCorrected = fYCorrected * fFy + fCy;
      fU = fXCorrected;
      fV = fYCorrected;
   }


   /****************************************/
   /****************************************/

   const UInt8 CDroneCamerasSystemDefaultSensor::UNDISTORT_ITERATIONS = 10;
   const Real CDroneCamerasSystemDefaultSensor::DEFAULT_TAG_SIDE_LENGTH = 0.0235f;

   /****************************************/
   /****************************************/

   REGISTER_SENSOR(CDroneCamerasSystemDefaultSensor,
                   "drone_cameras_system", "default",
                   "Michael Allwright [allsey87@gmail.com]",
                   "1.0",
                   "Camera sensor for the drone",
                   "Camera sensor for the drone\n",
                   "Under development");

   /****************************************/
   /****************************************/

}
