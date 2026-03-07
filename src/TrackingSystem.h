#pragma once

#include <string>
#include <vector>
#include <cstdint>

class FilamentApp;

bool tracking_openSerialPort(FilamentApp* app, const std::string& portName);
void tracking_closeSerialPort(FilamentApp* app);
bool tracking_openUdpPort(FilamentApp* app, int port);
void tracking_closeUdpPort(FilamentApp* app);
std::vector<std::string> tracking_enumerateComPorts();
void tracking_serialReadThread(FilamentApp* app);
void tracking_udpReadThread(FilamentApp* app);
bool tracking_parseVIPSMessage(FilamentApp* app, const uint8_t* data, size_t length);
bool tracking_parseFreeDMessage(FilamentApp* app, const uint8_t* data, size_t length);
void tracking_updateSceneFromTracking(FilamentApp* app, bool newWebcamFrame);
