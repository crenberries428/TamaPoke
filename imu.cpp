#include "imu.h"
#include "pin_config.h"
#include <Wire.h>
#include <SensorQMI8658.hpp>

static SensorQMI8658 qmi;
static bool imuOk = false;

bool imuBegin() {
  imuOk = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!imuOk) {
    Serial.println("QMI8658 not detected");
    return false;
  }
  // 62.5 Hz is plenty for a walking gait (~2 Hz) and the main loop cannot poll
  // faster than a render frame anyway. 4 g covers a heavy footfall.
  qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_62_5Hz);
  qmi.enableAccelerometer();
  return true;
}

bool imuReadAccel(float &x, float &y, float &z) {
  if (!imuOk || !qmi.getDataReady()) return false;
  return qmi.getAccelerometer(x, y, z);
}
