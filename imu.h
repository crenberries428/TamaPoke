#pragma once
#include <Arduino.h>

// QMI8658 6-axis IMU. Only the accelerometer is used (for step counting).
bool imuBegin();                              // false if the chip is not found
bool imuReadAccel(float &x, float &y, float &z);   // in g; false when no new sample is ready
