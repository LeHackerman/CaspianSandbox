//
// AgentIOCTL.h
//

//
// This header file contains all declarations shared between driver and user
// applications.
//

#pragma once

//
// The following value is arbitrarily chosen from the space defined by Microsoft
// as being "for non-Microsoft use"
//
#define FILE_DEVICE_AGENT 0xCF54

//
// Device control codes - values between 2048 and 4095 arbitrarily chosen
//
#define IOCTL_READY_TO_RECEIVE CTL_CODE(FILE_DEVICE_AGENT, 2049, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LAUNCH_EXECUTABLE CTL_CODE(FILE_DEVICE_AGENT, 1337, METHOD_BUFFERED, FILE_ANY_ACCESS)

