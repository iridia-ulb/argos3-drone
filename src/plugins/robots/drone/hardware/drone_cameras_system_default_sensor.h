/*
 * @file <argos3/plugins/robots/drone/hardware/drone_camera_system_default_sensor.h>
 *
 * @author Michael Allwright - <allsey87@gmail.com>
 * @author Weixu Zhu (Harry) - <zhuweixu_harry@126.com>
 */

#ifndef DRONE_CAMERA_SYSTEM_DEFAULT_SENSOR_H
#define DRONE_CAMERA_SYSTEM_DEFAULT_SENSOR_H

/* forward declarations */
namespace argos {
   class CDroneCameraSystemDefaultSensor;
}

struct apriltag_family;
struct apriltag_detector;
struct v4l2_buffer;

#include <array>
#include <chrono>

#include <apriltag/apriltag_pose.h>
#include <apriltag/common/image_u8.h>
#include <turbojpeg.h>

#include <argos3/core/utility/math/quaternion.h>
#include <argos3/core/utility/math/rng.h>
#include <argos3/core/utility/math/vector2.h>
#include <argos3/core/utility/math/vector3.h>

#include <argos3/plugins/robots/drone/hardware/robot.h>
#include <argos3/plugins/robots/drone/hardware/sensor.h>
#include <argos3/plugins/robots/drone/control_interface/ci_drone_cameras_system_sensor.h>

namespace argos {

   class CDroneCamerasSystemDefaultSensor : public CPhysicalSensor,
                                            public CCI_DroneCamerasSystemSensor {

   public:

      CDroneCamerasSystemDefaultSensor() {}

      virtual ~CDroneCamerasSystemDefaultSensor() {}

      virtual void Init(TConfigurationNode& t_tree);

      virtual void Destroy();

      virtual void Update();

      virtual void Visit(std::function<void(SInterface&)>) override;

   private:

      class SPhysicalInterface : public SInterface {
      public:
         SPhysicalInterface(const std::string& str_label,
                            const TConfiguration& t_configuration,
                            TConfigurationNode& t_interface,
                            const std::string& str_save_path);
         ~SPhysicalInterface();

         void Open();

         void Close();

         virtual void Enable() override;

         virtual void Disable() override;

         virtual void Update();

         ticpp::Element& GetMetadata() {
            return m_cMetadata;
         }

         const TConfiguration& GetConfiguration() const {
            return m_tConfiguration;
         };

      private:
         /* calibration data */
         struct SCalibration {
            CVector3 PositionError;
            CQuaternion OrientationError;
            CSquareMatrix<3> CameraMatrix;
            CVector3 DistortionK;
            CVector2 DistortionP;
         } m_sCalibration;
         void UndistortPixel(double& fU, double& fV);
         /* sensor configuration */
         std::array<UInt32, 2> m_arrCaptureResolution;
         std::array<UInt32, 2> m_arrProcessingResolution;
         std::array<UInt32, 2> m_arrProcessingOffset;
         /* camera parameters */
         UInt8 m_unCameraBrightness = 8;
         UInt8 m_unCameraContrast = 8;
         bool m_bCameraExposureAuto = true;
         UInt16 m_unCameraExposureAbsoluteTime = 1250;
         bool m_bExposureTimeSetFlag = false;
         /* camera position and orientation */
         TConfiguration m_tConfiguration;
         /* tag detector data */
         std::string m_strTagFamilyName;
         Real m_fTagSideLength;
         ::image_u8_t* m_ptImage;
         ::apriltag_family* m_ptTagFamily;
         ::apriltag_detector* m_ptTagDetector;
         ::apriltag_detection_info_t m_tTagDetectionInfo;
         /* jpeg decoder */
         ::tjhandle m_ptTurboJpegInstance;
         /* camera device handle */
         std::string m_strDevice;
         int m_nCameraHandle;
         /* buffers for capture */
         std::array<std::pair<UInt32, void*>, 2> m_arrBuffers;
         std::array<std::pair<UInt32, void*>, 2>::iterator m_itCurrentBuffer;
         std::array<std::pair<UInt32, void*>, 2>::iterator m_itNextBuffer;
         /* frame index */
         UInt32 m_unFrameIndex = 0;
         /* save frames */
         std::string m_strSavePath;
         ticpp::Element m_cMetadata;
      };

      /* time at initialization */
      std::chrono::steady_clock::time_point m_tpInit;

      /* vector of interfaces */
      std::vector<SPhysicalInterface> m_vecPhysicalInterfaces;

      /* path to save camera sensor data */
      std::string m_strSensorDataPath;

   private:
      static const UInt8 UNDISTORT_ITERATIONS;
      static const Real DEFAULT_TAG_SIDE_LENGTH;

      /****************************************/
      /****************************************/

   };
}

#endif
