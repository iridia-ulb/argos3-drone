/**
 * @file <argos3/plugins/robots/drone/hardware/drone_flight_system_default_actuator.h>
 *
 * @author Michael Allwright - <allsey87@gmail.com>
 * @author Sinan Oguz - <soguz.ankara@gmail.com>
 */

#ifndef DRONE_FLIGHT_SYSTEM_DEFAULT_ACTUATOR_H
#define DRONE_FLIGHT_SYSTEM_DEFAULT_ACTUATOR_H

namespace argos {
   class CDroneFlightSystemDefaultActuator;
   class CRobot;
}

#include <argos3/plugins/robots/drone/hardware/actuator.h>
#include <argos3/plugins/robots/drone/control_interface/ci_drone_flight_system_actuator.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include <argos3/plugins/robots/drone/hardware/mavlink/common/mavlink.h>
#pragma GCC diagnostic pop

namespace argos {

   class CDroneFlightSystemDefaultActuator : public CPhysicalActuator,
                                             public CCI_DroneFlightSystemActuator {

   public:

      CDroneFlightSystemDefaultActuator() {}

      virtual ~CDroneFlightSystemDefaultActuator() {}

      virtual void Init(TConfigurationNode& t_tree);

      virtual void Update();

      virtual bool Ready() override;

      virtual void SetOffboardMode(bool b_offboard_mode) override;

      virtual void Arm(bool b_arm, bool b_bypass_safety_checks) override;

   private:

      void Write(const mavlink_message_t& t_message);

   };
}

#endif
