/************************************************************************

Legrand MyHome / OpenWebNet Interface board driver for Domoticz (with LAN interface)
Date: 24-01-2016
Written by: Stéphane Lebrasseur

Date: 04-11-2016
Update by: Matteo Facchetti

Date: 13-09-2017
Update by: Marco Olivieri - Olix81 -

License: Public domain

************************************************************************/
#include "stdafx.h"
#include "OpenWebNetTCP.h"
#include "openwebnet/bt_openwebnet.h"

#include "../main/Logger.h"
#include "../main/Helper.h"
#include "../main/SQLHelper.h"
#include "../main/localtime_r.h"
#include "csocket.h"

#include <string.h>
#include "hardwaretypes.h"
#include "../main/RFXNames.h"
#include "../main/RFXtrx.h"

#define OPENWEBNET_HEARTBEAT_DELAY      1
#define OPENWEBNET_STATUS_NB_HEARTBEAT  600
#define OPENWEBNET_RETRY_DELAY          30
#define OPENWEBNET_POLL_INTERVAL        1000
#define OPENWEBNET_BUFFER_SIZE          1024
#define OPENWEBNET_SOCKET_SUCCESS       0

#define	SCAN_TIME_REQ_AUTO_UPDATE_POWER	(14400) // 4hour = 240min = 14400sec
#define SCAN_TIME_REQ_ENERGY_TOTALIZER	(900)	// 15min = 900sec

#define OPENWEBNET_GROUP_ID				0x00008000

#define OPENWEBNET_AUTOMATION					"AUTOMATION"
#define OPENWEBNET_LIGHT						"LIGHT"
#define OPENWEBNET_TEMPERATURE					"TEMPERATURE"
#define OPENWEBNET_BURGLAR_ALARM_SENSOR			"ALARM SENSOR ZONE"
#define OPENWEBNET_BURGLAR_ALARM_BATTERY		"ALARM BATTERY"
#define OPENWEBNET_BURGLAR_ALARM_NETWORK		"ALARM NETWORK"
#define OPENWEBNET_BURGLAR_ALARM_SYS_STATUS		"ALARM SYSTEM STATUS"
#define OPENWEBNET_BURGLAR_ALARM_SYS_ENGAGEMENT "ALARM SYSTEM ENGAGEMENT"
#define OPENWEBNET_CENPLUS            			"CEN PLUS"
#define OPENWEBNET_AUXILIARY					"AUXILIARY"
#define OPENWEBNET_DRY_CONTACT					"DRYCONTACT"
#define OPENWEBNET_ENERGY_MANAGEMENT			"ENERGY MANAGEMENT"
#define OPENWEBNET_SOUND_DIFFUSION				"SOUND DIFFUSION"

enum
{
	ID_DEV_BURGLAR_SYS_STATUS		= 17,	// ex 0xff
	ID_DEV_BURGLAR_NETWORK			= 18,	// ex 0xfe
	ID_DEV_BURGLAR_BATTERY			= 19,	// ex 0xfd
	ID_DEV_BURGLAR_SYS_ENGAGEMENT	= 20	// ex 0xfc
};


 /**
	 Create new hardware OpenWebNet instance
 **/
COpenWebNetTCP::COpenWebNetTCP(const int ID, const std::string &IPAddress, const unsigned short usIPPort, const std::string &ownPassword, const int ownScanTime) : m_szIPAddress(IPAddress)
{
	m_HwdID = ID;
	m_usIPPort = usIPPort;
	m_ownPassword = ownPassword;

	if (!ownScanTime)
		_log.Log(LOG_STATUS, "COpenWebNetTCP: scan devices DISABLED!");

	m_ownScanTime = ownScanTime;
	m_heartbeatcntr = OPENWEBNET_HEARTBEAT_DELAY;
	m_pStatusSocket = NULL;
}

/**
	destroys hardware OpenWebNet instance
**/
COpenWebNetTCP::~COpenWebNetTCP(void)
{
}

/**
	Start Hardware OpneWebNet Monitor/Worker Service
**/
bool COpenWebNetTCP::StartHardware()
{
	RequestStart();

	m_bIsStarted = true;
	mask_request_status = 0x1; // Set scan all devices
	LastScanTimeEnergy = LastScanTimeEnergyTot = 0;	// Force first request command

	//Start monitor thread
	m_monitorThread = std::make_shared<std::thread>(&COpenWebNetTCP::MonitorFrames, this);
	SetThreadName(m_monitorThread->native_handle(), "OpenWebNetTCPMF");

	//Start worker thread
	if (m_monitorThread != NULL) {
		m_heartbeatThread = std::make_shared<std::thread>(&COpenWebNetTCP::Do_Work, this);
		SetThreadName(m_heartbeatThread->native_handle(), "OpenWebNetTCPW");
	}

	return (m_monitorThread != NULL && m_heartbeatThread != NULL);
}

/**
	Stop Hardware OpenWebNet Monitor/Worker Service
**/
bool COpenWebNetTCP::StopHardware()
{
	RequestStop();

	//Force socket close to stop the blocking thread?
	if (m_pStatusSocket != NULL)
	{
		m_pStatusSocket->close();
	}

	try
	{
		if (m_monitorThread)
		{
			m_monitorThread->join();
			m_monitorThread.reset();
		}
	}
	catch (...)
	{
	}

	try
	{
		if (m_heartbeatThread)
		{
			m_heartbeatThread->join();
			m_heartbeatThread.reset();
		}
	}
	catch (...)
	{
	}

	m_bIsStarted = false;
	return true;
}

/**
	Close and delete the socket
**/
void COpenWebNetTCP::disconnect()
{
	if (m_pStatusSocket != NULL)
	{
		_log.Log(LOG_STATUS, "COpenWebNetTCP: disconnect");
		m_pStatusSocket->close();
		delete m_pStatusSocket;
		m_pStatusSocket = NULL;
	}
}


/**
   Check socket connection
**/
bool COpenWebNetTCP::isStatusSocketConnected()
{
	return m_pStatusSocket != NULL && m_pStatusSocket->getState() == csocket::CONNECTED;
};


/**
   Calculate 'nonce-hash' authentication
**/
uint32_t COpenWebNetTCP::ownCalcPass(const std::string &password, const std::string &nonce)
{
	uint32_t msr = 0x7FFFFFFF;
	uint32_t m_1 = (uint32_t)0xFFFFFFFF;
	uint32_t m_8 = (uint32_t)0xFFFFFFF8;
	uint32_t m_16 = (uint32_t)0xFFFFFFF0;
	uint32_t m_128 = (uint32_t)0xFFFFFF80;
	uint32_t m_16777216 = (uint32_t)0xFF000000;
	bool flag = true;
	uint32_t num1 = 0;
	uint32_t num2 = 0;
	uint32_t numx = 0;
	uint32_t length = 0;

	uint32_t idx;

	for (idx = 0; idx < nonce.length(); idx++)
	{
		if ((nonce[idx] >= '1') && (nonce[idx] <= '9'))
		{
			if (flag)
			{
				num2 = (uint32_t)atoi(password.c_str());
				flag = false;
			}
		}

		switch (nonce[idx])
		{
		case '1':
			num1 = num2 & m_128;
			num1 = num1 >> 1;
			num1 = num1 & msr;
			num1 = num1 >> 6;
			num2 = num2 << 25;
			num1 = num1 + num2;
			break;
		case '2':
			num1 = num2 & m_16;
			num1 = num1 >> 1;
			num1 = num1 & msr;
			num1 = num1 >> 3;
			num2 = num2 << 28;
			num1 = num1 + num2;
			break;
		case '3':
			num1 = num2 & m_8;
			num1 = num1 >> 1;
			num1 = num1 & msr;
			num1 = num1 >> 2;
			num2 = num2 << 29;
			num1 = num1 + num2;
			break;
		case '4':
			num1 = num2 << 1;
			num2 = num2 >> 1;
			num2 = num2 & msr;
			num2 = num2 >> 30;
			num1 = num1 + num2;
			break;
		case '5':
			num1 = num2 << 5;
			num2 = num2 >> 1;
			num2 = num2 & msr;
			num2 = num2 >> 26;
			num1 = num1 + num2;
			break;
		case '6':
			num1 = num2 << 12;
			num2 = num2 >> 1;
			num2 = num2 & msr;
			num2 = num2 >> 19;
			num1 = num1 + num2;
			break;
		case '7':
			num1 = num2 & 0xFF00;
			num1 = num1 + ((num2 & 0xFF) << 24);
			num1 = num1 + ((num2 & 0xFF0000) >> 16);
			num2 = num2 & m_16777216;
			num2 = num2 >> 1;
			num2 = num2 & msr;
			num2 = num2 >> 7;
			num1 = num1 + num2;
			break;
		case '8':
			num1 = num2 & 0xFFFF;
			num1 = num1 << 16;
			numx = num2 >> 1;
			numx = numx & msr;
			numx = numx >> 23;
			num1 = num1 + numx;
			num2 = num2 & 0xFF0000;
			num2 = num2 >> 1;
			num2 = num2 & msr;
			num2 = num2 >> 7;
			num1 = num1 + num2;
			break;
		case '9':
			num1 = ~num2;
			break;
		default:
			num1 = num2;
			break;
		}
		num2 = num1;
	}

	return (num1 & m_1);
}

/**
	Perform nonce-hash authentication
**/

bool COpenWebNetTCP::nonceHashAuthentication(csocket *connectionSocket)
{
	char databuffer[OPENWEBNET_BUFFER_SIZE];
	memset(databuffer, 0, OPENWEBNET_BUFFER_SIZE);
	int read = connectionSocket->read(databuffer, OPENWEBNET_BUFFER_SIZE, false);
	bt_openwebnet responseNonce(std::string(databuffer, read));
	if (responseNonce.IsPwdFrame())
	{
		std::stringstream frame;
		uint32_t ownHash;

		if (!m_ownPassword.length())
		{
			_log.Log(LOG_STATUS, "COpenWebNetTCP: no password set for a unofficial bticino gateway");
			return false;
		}

		/** calculate nonce-hash **/
		ownHash = ownCalcPass(m_ownPassword, responseNonce.Extract_who());
		/** write frame with nonce-hash **/
		frame << "*#";
		frame << ownHash;
		frame << "##";

		int bytesWritten = connectionSocket->write(frame.str().c_str(), frame.str().length());
		if (bytesWritten != frame.str().length()) {
			_log.Log(LOG_ERROR, "COpenWebNetTCP: partial write");
		}

		/** Open password for test **/
		memset(databuffer, 0, OPENWEBNET_BUFFER_SIZE);
		read = connectionSocket->read(databuffer, OPENWEBNET_BUFFER_SIZE, false);
		bt_openwebnet responseNonce2(std::string(databuffer, read));
		if (responseNonce2.IsOKFrame()) return true;
		_log.Log(LOG_ERROR, "COpenWebNetTCP: authentication ERROR!");
		return false;
	}
	else if (responseNonce.IsOKFrame())
	{
		return true;
	}
	_log.Log(LOG_STATUS, "COpenWebNetTCP: ERROR_FRAME? %d", responseNonce.frame_type);
	return false;
}

/**
   Connection to the gateway OpenWebNet
**/
csocket* COpenWebNetTCP::connectGwOwn(const char *connectionMode)
{
	if (m_szIPAddress.size() == 0 || m_usIPPort == 0 || m_usIPPort > 65535)
	{
		_log.Log(LOG_ERROR, "COpenWebNetTCP: Cannot connect to gateway, empty  IP Address or Port");
		return NULL;
	}

	/* new socket for command and session connection */
	csocket *connectionSocket = new csocket();

	connectionSocket->connect(m_szIPAddress.c_str(), m_usIPPort);
	if (connectionSocket->getState() != csocket::CONNECTED)
	{
		_log.Log(LOG_ERROR, "COpenWebNetTCP: Cannot connect to gateway, Unable to connect to specified IP Address on specified Port");
		disconnect();  // disconnet socket if present
		return NULL;
	}

	char databuffer[OPENWEBNET_BUFFER_SIZE];
	memset(databuffer, 0, OPENWEBNET_BUFFER_SIZE);
	int read = connectionSocket->read(databuffer, OPENWEBNET_BUFFER_SIZE, false);
	bt_openwebnet responseSession(std::string(databuffer, read));
	if (!responseSession.IsOKFrame())
	{
		_log.Log(LOG_STATUS, "COpenWebNetTCP: failed to begin session, NACK received (%s:%d)-> %s", m_szIPAddress.c_str(), m_usIPPort, databuffer);
		disconnect();  // disconnet socket if present
		return NULL;
	}

	int bytesWritten = connectionSocket->write(connectionMode, strlen(connectionMode));
	if (bytesWritten != strlen(connectionMode)) {
		_log.Log(LOG_ERROR, "COpenWebNetTCP: partial write");
	}

	if (!nonceHashAuthentication(connectionSocket)) return NULL;

	return connectionSocket;
}

/**
	Thread Monitor: get update from the OpenWebNet gateway and add new devices if necessary
**/
void COpenWebNetTCP::MonitorFrames()
{
	while (!IsStopRequested(0))
	{
		if (!isStatusSocketConnected())
		{
			disconnect();  // disconnet socket if present
			time_t atime = time(NULL);
			if ((atime%OPENWEBNET_RETRY_DELAY) == 0)
			{
				if ((m_pStatusSocket = connectGwOwn(OPENWEBNET_EVENT_SESSION)))
				{
					// Monitor session correctly open
					_log.Log(LOG_STATUS, "COpenWebNetTCP: Monitor session connected to: %s:%d", m_szIPAddress.c_str(), m_usIPPort);
					sOnConnected(this);
				}
				else
				{
					_log.Log(LOG_STATUS, "COpenWebNetTCP: TCP/IP monitor not connected, retrying in %d seconds...", OPENWEBNET_RETRY_DELAY);
					sleep_seconds(1);
				}
			}
		}
		else
		{
			// Connected
			bool bIsDataReadable = true;
			m_pStatusSocket->canRead(&bIsDataReadable, 3.0f);
			if (bIsDataReadable)
			{
				char data[OPENWEBNET_BUFFER_SIZE];
				memset(data, 0, OPENWEBNET_BUFFER_SIZE);
				int bread = m_pStatusSocket->read(data, OPENWEBNET_BUFFER_SIZE, false);

				if (IsStopRequested(0))
					break;

				if ((bread == 0) || (bread < 0)) {
					_log.Log(LOG_ERROR, "COpenWebNetTCP: TCP/IP monitor connection closed!");
					disconnect();  // disconnet socket if present
				}
				else
				{
					std::lock_guard<std::mutex> l(readQueueMutex);
					std::vector<bt_openwebnet> responses;
					ParseData(data, bread, responses);

					for (std::vector<bt_openwebnet>::iterator iter = responses.begin(); iter != responses.end(); iter++) {
						if (iter->IsNormalFrame() || iter->IsMeasureFrame())
						{
							_log.Log(LOG_STATUS, "COpenWebNetTCP: received=%s", bt_openwebnet::frameToString(*iter).c_str());
							UpdateDeviceValue(iter);
						}
						//else
						//    _log.Log(LOG_ERROR, "COpenWebNetTCP: SKIPPED FRAME=%s", frameToString(*iter).c_str());
					}
				}
			}
			else
				sleep_milliseconds(100);
		}
	}
	if (isStatusSocketConnected())
	{
		try {
			disconnect();  // disconnet socket if present
		}
		catch (...)
		{
			//Don't throw from a Stop command
		}
	}

	_log.Log(LOG_STATUS, "COpenWebNetTCP: TCP/IP monitor worker stopped...");
}

/**
	Insert/Update temperature device
**/
void COpenWebNetTCP::UpdateTemp(const int who, const int where, float fval, const int iInterface, const int BatteryLevel, const char *devname)
{
	/**
		NOTE:	interface id (bus identifier) go in 'ID' field
				ID is: ((who << 12) & 0xC000) | (iInterface & 0x3c00) (where & 0x3FF)
	**/

	//zone are max 99,, every zone can have 8 slave sensor. Slave sensor address. YZZ: y as slave address (1-8) ,zz zone number (1-99)
	// so who is always 4, iInterface is 0-9, where 0x001-0x383 (1 to 899)
	int cnode = ((who << 12) & 0xC000) | (iInterface & 0x3c00) | (where & 0x3FF);
	SendTempSensor(cnode, BatteryLevel, fval, devname);
}

/**
	Update temperature setpoint
**/
void COpenWebNetTCP::UpdateSetPoint(const int who, const int where, float fval, const int iInterface, const char *devname)
{
	/**
		NOTE:	interface id (bus identifier) go in 'ID' field
				ID is: ((who << 16) | (iInterface << 8) | where)

				where is setpoint zone (1 - 99)
	**/
	SendSetPointSensor((who & 0xFF), (iInterface & 0xff), (where & 0xFF), fval, devname);
}

/**
Get last Meter Usage
**/
bool COpenWebNetTCP::GetValueMeter(const int NodeID, const int ChildID, double *usage, double *energy)
{
	int dID = (NodeID << 8) | ChildID;
	char szTmp[30];
	sprintf(szTmp, "%08X", dID);

	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT sValue FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q') AND (Type==%d) AND (Subtype==%d)",
		m_HwdID, szTmp, int(pTypeGeneral), int(sTypeKwh));
	if (result.size() >= 1)
	{
		std::string sup, sValue = result[0][0].c_str();

		if (usage)
			*usage = (float)atof(sValue.c_str());

		if (energy)
		{
			size_t pos = sValue.find(';', 0);
			if (pos >= 0)
			{
				sup = sValue.substr(pos + 1);
				*energy = (float)atof(sup.c_str());
			}
		}

		return true;
	}
	return false; // not found
}

/**
	Update Active power usage
**/
void COpenWebNetTCP::UpdatePower(const int who, const int where, double fval, const int iInterface, const int BatteryLevel, const char *devname)
{
	/**
		NOTE:	interface id (bus identifier) go in 'ID' field
				ID is: ((iInterface << 16) | (who << 8) | where)
	**/
	double energy = 0.;

	int NodeId = (iInterface << 8) | who;
	GetValueMeter(NodeId, where, NULL, &energy);
	SendKwhMeter(NodeId, where, BatteryLevel, fval, (energy / 1000.), devname);
}


/**
	Update total energy
**/
void COpenWebNetTCP::UpdateEnergy(const int who, const int where, double fval, const int iInterface, const int BatteryLevel, const char *devname)
{
	/**
		NOTE:	interface id (bus identifier) go in 'ID' field
				ID is: ((iInterface << 16) | (who << 8) | where)
	**/

	double usage = 0.;
	int NodeId = (iInterface << 8) | who;
	GetValueMeter(NodeId, where, &usage, NULL);
	SendKwhMeter(NodeId, where, BatteryLevel, usage, fval, devname);
}


/**
	Insert/Update Alarm system and sensor
**/
void COpenWebNetTCP::UpdateAlarm(const int who, const int where, const int Command, const char* sCommand, const int iInterface, const int BatteryLevel, const char* devname)
{
	/**
		NOTE:	interface id (bus identifier) go in 'ID' field

		each interface can occupy 1 to 20 IDs.
		so you can calculate the ID:
		ID + interface * 20 so for example:

		iInterface = 0, Id from 1 to 20
		iInterface = 1, Id from 21 to 40
		iInterface = 2, Id from 41 to 60
		and so on..
	**/

	//make device ID
	//int NodeID = (((int)who << 16) & 0xFFFF0000) | (((int)where) & 0x0000FFFF);
	int NodeID = (where & 0xff) + iInterface * 20;

	char szIdx[10];
	sprintf(szIdx, "%u", NodeID);

	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT nValue FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%s')", m_HwdID, szIdx);
	if (!result.empty())
	{
		//check if we have a change, if not do not update it
		int nvalue = atoi(result[0][0].c_str());
		if (Command == -1 || nvalue == Command) return; // update not necessary
	}

	SendAlertSensor(NodeID, BatteryLevel, Command, sCommand, devname);
}

/**
	Insert/Update blinds device
**/
void COpenWebNetTCP::UpdateBlinds(const int who, const int where, const int Command, const int iInterface, const int iLevel, const int BatteryLevel, const char *devname)
{
	//NOTE: interface id (bus identifier) go in 'Unit' field
	//make device ID
	int NodeID = (((int)who << 16) & 0xFFFF0000) | (((int)where) & 0x0000FFFF);

	/* insert switch type */
	char szIdx[10];
	sprintf(szIdx, "%07X", NodeID);

	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT ID,SwitchType FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%s') AND (Unit==%d)", m_HwdID, szIdx, iInterface);
	if (result.empty())
	{
		int switch_type = (iLevel < 0) ? STYPE_VenetianBlindsEU : STYPE_BlindsPercentageInverted;
		m_sql.InsertDevice(m_HwdID, szIdx, iInterface, pTypeLighting2, sTypeAC, switch_type, 0, "", devname);
	}

	// if (iLevel < 0)  is a Normal Frame and device is standard
	// if (iLevel >= 0) is a Meseaure Frame (percentual) and device is Advanced
	/*TODO: verify level value for Advanced */
	double level = (iLevel < 0) ? 0. : iLevel;
	if (level == 100.) level -= 6.25;
	SendSwitch(NodeID, iInterface, BatteryLevel, (bool)Command, level, devname);
}


/**
	Insert/Update  CEN PLUS
**/
void COpenWebNetTCP::UpdateCenPlus(const int who, const int where, const int Command, const int iAppValue, const int what, const int iInterface, const int BatteryLevel, const char *devname)
{
	//NOTE: interface id (bus identifier) go in 'Unit' field
	//make device ID
	int NodeID = (int)((((int)who << 16) & 0xFFFF0000) | (((int)(where + (iAppValue * 2) + (what * 3))) & 0x0000FFFF));
	SendSwitch(NodeID, iInterface, BatteryLevel, (bool)Command, 0, devname);
}

/**
	Insert/Update  switch device
**/
void COpenWebNetTCP::UpdateSwitch(const int who, const int where, const int what, const int iInterface, const int BatteryLevel, const char* devname)
{
	//NOTE: interface id (bus identifier) go in 'Unit' field
	//make device ID
	int NodeID = (((int)who << 16) & 0xFFFF0000) | (((int)where) & 0x0000FFFF);

	/* insert switch type */
	char szIdx[10];
	sprintf(szIdx, "%07X", NodeID);
	std::vector<std::vector<std::string> > result;
	result = m_sql.safe_query("SELECT ID,SwitchType FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%s') AND (Unit==%d)", m_HwdID, szIdx, iInterface);

	if (result.empty())
	{
		if ((who == WHO_CEN_PLUS_DRY_CONTACT_IR_DETECTION) || (who == (WHO_TEMPERATURE_CONTROL + 500)) || (who == (WHO_TEMPERATURE_CONTROL + 600)))
		{
			// First insert, set SwitchType = STYPE_Contact, so we have a correct contact device
			m_sql.InsertDevice(m_HwdID, szIdx, iInterface, pTypeLighting2, sTypeAC, STYPE_Contact, 0, "Unavailable", devname);
		}
	}

	double level = (what > 1) ? (what * 10.) : 0;
	if (level == 100.) level -= 6.25;
	SendSwitch(NodeID, iInterface, BatteryLevel, (bool)what, level, devname);
}

/**
	Insert/Update device
**/
void COpenWebNetTCP::UpdateDeviceValue(std::vector<bt_openwebnet>::iterator iter)
{
	std::string who = iter->Extract_who();
	std::string where = iter->Extract_where();
	std::string what = iter->Extract_what();
	std::vector<std::string> whereParam = iter->Extract_whereParameters();
	std::vector<std::string> whatParam = iter->Extract_whatParameters();
	std::string dimension = iter->Extract_dimension();
	std::string value = iter->Extract_value(0);
	std::string sInterface = iter->Extract_interface();
	std::string devname, sCommand;
	int iAppValue, iWhere, iLevel;

	switch (atoi(who.c_str())) {
	case WHO_LIGHTING:									// 1
		if (!iter->IsNormalFrame())
		{
			_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s not normal frame! -> frame_type=%d", who.c_str(), iter->frame_type);
			return;
		}

		iAppValue = atoi(what.c_str());
		if (iAppValue == 1000) // What = 1000 (Command translation)
			iAppValue = atoi(whatParam[0].c_str());

		iWhere = atoi(where.c_str());

		devname = OPENWEBNET_LIGHT;
		if ((!whereParam.empty()) && (iWhere == 0))
		{
			/* GROUP light device */
			iWhere = atoi(whereParam[0].c_str()) + OPENWEBNET_GROUP_ID;
			devname += " GROUP " + whereParam[0];
		}
		else if (iWhere < MAX_WHERE_AREA)
		{
			/* AREA light device */
			mask_request_status |= (0x1 << iWhere); // Gen or area, need a refresh devices status
			if (iWhere == 0)
				devname += " GEN " + where;
			else
				devname += " AREA " + where;
		}
		else
		{
			/* Normal light device */
			devname += " " + where;
		}

		UpdateSwitch(WHO_LIGHTING, iWhere, iAppValue, atoi(sInterface.c_str()), 255, devname.c_str());
		break;
	case WHO_AUTOMATION:								// 2
		if (!iter->IsNormalFrame() && !iter->IsMeasureFrame())
		{
			_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s frame error!", who.c_str());
			return;
		}
		if (iter->IsMeasureFrame()) // Advanced motor actuator (percentual) *#2*19*10*10*65*000*0##
		{
			std::string level = iter->Extract_value(1);
			iLevel = atoi(level.c_str());
			iAppValue = atoi(value.c_str());

			if (iAppValue == 1000) // What = 1000 (Command translation)
				iAppValue = atoi(whatParam[0].c_str());

			iWhere = atoi(where.c_str());

			devname = OPENWEBNET_AUTOMATION;
			if ((!whereParam.empty()) && (iWhere == 0))
			{
				/* GROUP automation device */
				iWhere = atoi(whereParam[0].c_str()) + OPENWEBNET_GROUP_ID;
				devname += " GROUP " + whereParam[0];
			}
			else if (iWhere < MAX_WHERE_AREA)
			{
				/* AREA automation device */
				mask_request_status |= (0x1 << iWhere); // Gen or area, need a refresh devices status
				if (iWhere == 0)
					devname += " GEN " + where;
				else
					devname += " AREA " + where;
			}
			else
			{
				/* Normal automation device */
				devname += " " + where;
			}
			UpdateBlinds(WHO_AUTOMATION, iWhere, iAppValue, atoi(sInterface.c_str()), iLevel, 255, devname.c_str());
		}
		if (iter->IsNormalFrame())
		{
			iAppValue = atoi(what.c_str());
			if (iAppValue == 1000) // What = 1000 (Command translation)
				iAppValue = atoi(whatParam[0].c_str());

			switch (iAppValue)
			{
			case AUTOMATION_WHAT_STOP:  // 0
				iAppValue = gswitch_sStop;
				break;
			case AUTOMATION_WHAT_UP:    // 1
				iAppValue = gswitch_sOff;
				break;
			case AUTOMATION_WHAT_DOWN:  // 2
				iAppValue = gswitch_sOn;
				break;
			default:
				_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s, What=%s invalid!", who.c_str(), what.c_str());
				return;
			}

			iWhere = atoi(where.c_str());

			devname = OPENWEBNET_AUTOMATION;
			if ((!whereParam.empty()) && (iWhere == 0))
			{
				/* GROUP automation device */
				iWhere = atoi(whereParam[0].c_str()) + OPENWEBNET_GROUP_ID;
				devname += " GROUP " + whereParam[0];
			}
			else if (iWhere < MAX_WHERE_AREA)
			{
				/* AREA automation device */
				mask_request_status |= (0x1 << iWhere); // Gen or area, need a refresh devices status
				if (iWhere == 0)
					devname += " GEN " + where;
				else
					devname += " AREA " + where;
			}
			else
			{
				/* Normal automation device */
				devname += " " + where;
			}

			UpdateBlinds(WHO_AUTOMATION, iWhere, iAppValue, atoi(sInterface.c_str()), -1, 255, devname.c_str());
		}
		break;
	case WHO_TEMPERATURE_CONTROL:
		if (!iter->IsMeasureFrame())
		{
			if (iter->IsNormalFrame())
				_log.Log(LOG_STATUS, "COpenWebNetTCP: who=%s, what:%s, where=%s not yet supported", who.c_str(), what.c_str(), where.c_str());
			else
				_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s frame error!", who.c_str());
			return;
		}
		// 4: this is a openwebnet termoregulation update/poll messagge, setup devname
		devname = OPENWEBNET_TEMPERATURE;
		devname += " " + where;
		switch (atoi(dimension.c_str()))
		{
		case TEMPERATURE_CONTROL_DIMENSION_TEMPERATURE:
			UpdateTemp(WHO_TEMPERATURE_CONTROL, atoi(where.c_str()), static_cast<float>(atof(value.c_str()) / 10.), atoi(sInterface.c_str()), 255, devname.c_str());
			break;
		case TEMPERATURE_CONTROL_DIMENSION_VALVES_STATUS:
			devname += " Valves";
			UpdateSwitch(WHO_TEMPERATURE_CONTROL + 600, atoi(where.c_str()), atoi(value.c_str()), atoi(sInterface.c_str()), 255, devname.c_str());
			break;
		case TEMPERATURE_CONTROL_DIMENSION_ACTUATOR_STATUS:
			devname += " Actuator";
			UpdateSwitch(WHO_TEMPERATURE_CONTROL + 500, atoi(where.c_str()), atoi(value.c_str()), atoi(sInterface.c_str()), 255, devname.c_str());
			break;
		case TEMPERATURE_CONTROL_DIMENSION_COMPLETE_PROBE_STATUS:
			devname += " Setpoint";
			UpdateSetPoint(WHO_TEMPERATURE_CONTROL, atoi(where.c_str()), static_cast<float>(atof(value.c_str()) / 10.), atoi(sInterface.c_str()), devname.c_str());
			break;
		default:
			_log.Log(LOG_STATUS, "COpenWebNetTCP: who=%s, where=%s, dimension=%s not yet supported", who.c_str(), where.c_str(), dimension.c_str());
			break;
		}
		break;

	case WHO_BURGLAR_ALARM:                         // 5
		if (!iter->IsNormalFrame())
		{
			_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s not normal frame! -> frame_type=%d", who.c_str(), iter->frame_type);
			return;
		}
		
		switch (atoi(what.c_str())) {
		case 0:         //maintenace
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm in Maintenance");
			iWhere = ID_DEV_BURGLAR_SYS_STATUS; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_SYS_STATUS;
			sCommand = "Maintenance";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 2, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;
		case 1:         //active
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Active");
			iWhere = ID_DEV_BURGLAR_SYS_STATUS; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_SYS_STATUS;
			sCommand = "Active";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 1, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;
		case 2:         //disabled
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Inactive");
			iWhere = ID_DEV_BURGLAR_SYS_STATUS; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_SYS_STATUS;
			sCommand = "Inactive";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 3, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 4:         //battery fault
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Battery Fault");
			iWhere = ID_DEV_BURGLAR_BATTERY; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_BATTERY;
			sCommand = "Battery Fault";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 4, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 5:         //battery ok
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Battery OK");
			iWhere = ID_DEV_BURGLAR_BATTERY; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_BATTERY;
			sCommand = "Battery Ok";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 1, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 6:			//no network
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm no network");
			iWhere = ID_DEV_BURGLAR_NETWORK; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_NETWORK;
			sCommand = "No network";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 4, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 7:			//network ok
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm network ok");
			iWhere = ID_DEV_BURGLAR_NETWORK; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_NETWORK;
			sCommand = "Network OK";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 1, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 8: 	//engaged
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Engaged");
			iWhere = ID_DEV_BURGLAR_SYS_ENGAGEMENT; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_SYS_ENGAGEMENT;
			sCommand = "Engaged";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 1, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 9:         //disengaged
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Disengaged");
			iWhere = ID_DEV_BURGLAR_SYS_ENGAGEMENT; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_SYS_ENGAGEMENT;
			sCommand = "DisEngaged";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 0, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 10:         //battery Unloads
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Battery Unloads");
			iWhere = ID_DEV_BURGLAR_BATTERY; // force where because not exist
			devname = OPENWEBNET_BURGLAR_ALARM_BATTERY;
			sCommand = "Battery Unloads";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 4, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 11:         // zone N Active
			iWhere = atoi(whereParam[0].c_str());
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Zone %d Active",iWhere);
			devname = OPENWEBNET_BURGLAR_ALARM_SENSOR;
			devname += " " + whereParam[0];
			sCommand = "Active";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 1, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 15:         //zone N INTRUSION ALARM
			iWhere = atoi(whereParam[0].c_str());
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Zone %d INTRUSION ALARM", iWhere);
			devname = OPENWEBNET_BURGLAR_ALARM_SENSOR;
			devname += " " + whereParam[0];
			sCommand = "Intrusion";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 4, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 18:         // zone N Not Active
			iWhere = atoi(whereParam[0].c_str());
			//_log.Log(LOG_STATUS, "COpenWebNetTCP: Alarm Zone %d Not active", iWhere);
			devname = OPENWEBNET_BURGLAR_ALARM_SENSOR;
			devname += " " + whereParam[0];
			sCommand = "Inactive";
			UpdateAlarm(WHO_BURGLAR_ALARM, iWhere, 0, sCommand.c_str(), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		default:
			_log.Log(LOG_STATUS, "COpenWebNetTCP: who=%s, where=%s, dimension=%s not yet supported", who.c_str(), where.c_str(), dimension.c_str());
			break;
		}
		break;
	case WHO_AUXILIARY:                             // 9
		/**
			example:

			*9*what*where##

			what:   0 = OFF
					1 = ON
			where:  1 to 9 (AUX channel)
		**/
		if (!iter->IsNormalFrame())
		{
			_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s frame error!", who.c_str());
			return;
		}

		devname = OPENWEBNET_AUXILIARY;
		devname += " " + where;

		UpdateSwitch(WHO_AUXILIARY, atoi(where.c_str()), atoi(what.c_str()), atoi(sInterface.c_str()), 255, devname.c_str());
		break;
	case WHO_CEN_PLUS_DRY_CONTACT_IR_DETECTION:              // 25
		if (!iter->IsNormalFrame())
		{
			_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s not normal frame! -> frame_type=%d", who.c_str(), iter->frame_type);
			return;
		}


		switch (atoi(what.c_str())) { //CEN PLUS / DRY CONTACT / IR DETECTION

		case 21:         //Short pressure
			iWhere = atoi(where.c_str());
			iAppValue = atoi(whatParam[0].c_str());
			_log.Log(LOG_STATUS, "COpenWebNetTCP: CEN PLUS Short pressure %d Button %d", iWhere, iAppValue);
			devname = OPENWEBNET_CENPLUS;
			devname += " " + where + " Short Press Button " + whatParam[0].c_str();
			UpdateCenPlus(WHO_CEN_PLUS_DRY_CONTACT_IR_DETECTION, iWhere, 1, iAppValue, atoi(what.c_str()), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 22:         //Start of extended pressure
			_log.Log(LOG_STATUS, "COpenWebNetTCP: CEN Start of extended pressure");
			break;

		case 23:         //Extended pressure
			_log.Log(LOG_STATUS, "COpenWebNetTCP: CEN Extended pressure");
			break;

		case 24:         //End of Extended pressure
			iWhere = atoi(where.c_str());
			iAppValue = atoi(whatParam[0].c_str());

			_log.Log(LOG_STATUS, "COpenWebNetTCP: CEN PLUS Long pressure %d Button %d", iWhere, iAppValue);
			devname = OPENWEBNET_CENPLUS;
			devname += " " + where + " Long Press Button " + whatParam[0].c_str();
			UpdateCenPlus(WHO_CEN_PLUS_DRY_CONTACT_IR_DETECTION, iWhere, 1, iAppValue, atoi(what.c_str()), atoi(sInterface.c_str()), 255, devname.c_str());
			break;

		case 31:
		case 32:
			if (where.substr(0, 1) != "3")
			{
				_log.Log(LOG_ERROR, "COpenWebNetTCP: Where=%s is not correct for who=%s", where.c_str(), who.c_str());
				return;
			}

			devname = OPENWEBNET_DRY_CONTACT;
			devname += " " + where.substr(1);
			iWhere = atoi(where.substr(1).c_str());

			iAppValue = atoi(what.c_str());
			if (iAppValue == DRY_CONTACT_IR_DETECTION_WHAT_ON)
				iAppValue = 1;
			else
				iAppValue = 0;

			UpdateSwitch(WHO_CEN_PLUS_DRY_CONTACT_IR_DETECTION, iWhere, iAppValue, atoi(sInterface.c_str()), 255, devname.c_str());
			break;
		default:
			_log.Log(LOG_ERROR, "COpenWebNetTCP: What=%s is not correct for who=%s", what.c_str(), who.c_str());
			return;
		}
		break;
	case WHO_ENERGY_MANAGEMENT:                     // 18
		if (!iter->IsMeasureFrame())
		{
			if (iter->IsNormalFrame())
				_log.Log(LOG_STATUS, "COpenWebNetTCP: who=%s, what:%s, where=%s not yet supported", who.c_str(), what.c_str(), where.c_str());
			else
				_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s frame error!", who.c_str());
			return;
		}
		devname = OPENWEBNET_ENERGY_MANAGEMENT;
		devname += " " + where;
		switch (atoi(dimension.c_str()))
		{
		case ENERGY_MANAGEMENT_DIMENSION_ACTIVE_POWER:
			UpdatePower(WHO_ENERGY_MANAGEMENT, atoi(where.c_str()), static_cast<float>(atof(value.c_str())), atoi(sInterface.c_str()), 255, devname.c_str());
			break;
		case ENERGY_MANAGEMENT_DIMENSION_ENERGY_TOTALIZER:
			UpdateEnergy(WHO_ENERGY_MANAGEMENT, atoi(where.c_str()), static_cast<float>(atof(value.c_str()) / 1000.), atoi(sInterface.c_str()), 255, devname.c_str());
			break;
		default:
			_log.Log(LOG_STATUS, "COpenWebNetTCP: who=%s, where=%s, dimension=%s not yet supported", who.c_str(), where.c_str(), dimension.c_str());
			break;
		}
		break;
	case WHO_SCENARIO:                              // 0
	case WHO_LOAD_CONTROL:                          // 3
	case WHO_DOOR_ENTRY_SYSTEM:                     // 6
	case WHO_MULTIMEDIA:                            // 7
	case WHO_GATEWAY_INTERFACES_MANAGEMENT:         // 13
	case WHO_LIGHT_SHUTTER_ACTUATOR_LOCK:           // 14
	case WHO_SCENARIO_SCHEDULER_SWITCH:             // 15
	case WHO_AUDIO:                                 // 16
	case WHO_SCENARIO_PROGRAMMING:                  // 17
	case WHO_SOUND_DIFFUSION:						// 22
	case WHO_LIHGTING_MANAGEMENT:                   // 24
	case WHO_ZIGBEE_DIAGNOSTIC:                     // 1000
	case WHO_AUTOMATIC_DIAGNOSTIC:                  // 1001
	case WHO_THERMOREGULATION_DIAGNOSTIC_FAILURES:  // 1004
	case WHO_DEVICE_DIAGNOSTIC:                     // 1013
	case WHO_ENERGY_MANAGEMENT_DIAGNOSTIC:			// 1018
		_log.Log(LOG_ERROR, "COpenWebNetTCP: Who=%s not yet supported!", who.c_str());
		return;
	default:
		_log.Log(LOG_ERROR, "COpenWebNetTCP: ERROR Who=%s not exist!", who.c_str());
		return;
	}
}


/**
   Convert domoticz command in a OpenWebNet command, then send it to device
**/
bool COpenWebNetTCP::WriteToHardware(const char *pdata, const unsigned char length)
{
	//_tGeneralSwitch *pCmd = (_tGeneralSwitch*)pdata;
	const tRBUF* pCmd = reinterpret_cast<const tRBUF*>(pdata);
	
	unsigned char packetlength = pCmd->ICMND.packetlength;
	unsigned char packettype = pCmd->ICMND.packettype;
	unsigned char subtype = pCmd->ICMND.subtype;;

	int who = 0;
	int what = 0;
	int where = 0;
	int iInterface = 0;
	int Level = -1;
	std::vector<std::vector<std::string> > result;


	char szIdx[10];

	// Test packet type
	switch (packettype) {
	//case pTypeGeneralSwitch:
	case pTypeLighting2:
		// Test general switch subtype
		iInterface = pCmd->LIGHTING2.unitcode;
		who = (((int)pCmd->LIGHTING2.id1 << 8) & 0xFF00) | (((int)pCmd->LIGHTING2.id2) & 0x00FF);
		where = (((int)pCmd->LIGHTING2.id3 << 8) & 0xFF00) | (((int)pCmd->LIGHTING2.id4) & 0x00FF);

		switch (who) {
		case WHO_AUTOMATION:
			//Blinds/Window command
			sprintf(szIdx, "%07X", ((who << 16) & 0xffff0000) | (where & 0x0000ffff));

			result = m_sql.safe_query("SELECT nValue FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%s') AND (SwitchType==%d)",  //*******is there a better method for get
				m_HwdID, szIdx, STYPE_BlindsPercentageInverted);																		   //*******SUBtype (STYPE_BlindsPercentageInverted) ??

			if (result.empty())// from a normal button
			{
				if (pCmd->LIGHTING2.cmnd == light2_sOff)
				{
					what = AUTOMATION_WHAT_UP;
				}
				else if (pCmd->LIGHTING2.cmnd == light2_sOn)
				{
					what = AUTOMATION_WHAT_DOWN;
				}
				else if (pCmd->LIGHTING2.cmnd == gswitch_sStop)
				{
					what = AUTOMATION_WHAT_STOP;
				}
			}
			else // advanced button
			{
				if (pCmd->LIGHTING2.cmnd == light2_sOn)
				{
					what = AUTOMATION_WHAT_UP;
				}
				else if (pCmd->LIGHTING2.cmnd == light2_sOff)
				{
					what = AUTOMATION_WHAT_DOWN;
				}

				else if (pCmd->LIGHTING2.cmnd == light2_sSetLevel)
				{
					// setting level of Blinds
					Level = int(pCmd->LIGHTING2.level); // uint8_t, always >= 0
				}
			}

			break;
		case WHO_LIGHTING:
			//Light/Switch command
			if (pCmd->LIGHTING2.cmnd == light2_sOff)
			{
				what = LIGHTING_WHAT_OFF;
			}
			else if (pCmd->LIGHTING2.cmnd == light2_sOn)
			{
				what = LIGHTING_WHAT_ON;
			}
			else if (pCmd->LIGHTING2.cmnd == light2_sSetLevel)
			{
				// setting level of dimmer
				if (pCmd->LIGHTING2.level != 0)
				{
					BYTE level = pCmd->LIGHTING2.level * 100 / 15;
					if (level < 20) level = 20; // minimum value after 0
					what = int((level + 5) / 10);
				}
				else
				{
					what = LIGHTING_WHAT_OFF;
				}
			}
			break;
		case WHO_AUXILIARY:
			//Auxiliary command
			if (pCmd->LIGHTING2.cmnd == light2_sOff)
			{
				what = AUXILIARY_WHAT_OFF;
			}
			else if (pCmd->LIGHTING2.cmnd == light2_sOn)
			{
				what = AUXILIARY_WHAT_ON;
			}
			break;
		default:
			break;
		}

		//int used = 1;
		if (!FindDevice(who, where, iInterface, NULL)) {
			_log.Log(LOG_ERROR, "COpenWebNetTCP: command received for unknown device : %d/%d", who, where);
			return false;
		}

		break;
	default:
		_log.Log(LOG_STATUS, "COpenWebNetTCP unknown command: packettype=%d subtype=%d", packettype, subtype);
		return false;
	}


	if (iInterface == 0) {
		//local bus
		if (Level >= 0) { //Advanced Blind
			std::vector<bt_openwebnet> responses;
			bt_openwebnet request;
			std::stringstream dimensionStr;
			dimensionStr << AUTOMATION_DIMENSION_GOTO_LEVEL;
			std::string sDimension = dimensionStr.str();

			std::stringstream whoStr;
			whoStr << who;
			std::string sWho = whoStr.str();

			std::stringstream whereStr;
			whereStr << where;
			std::string sWhere = whereStr.str();

			std::stringstream levelStr;
			levelStr << Level;

			std::vector<std::string> value;
			value.clear();
			value.push_back("001");
			value.push_back(levelStr.str());
			request.CreateWrDimensionMsgOpen2(whoStr.str(), whereStr.str(), dimensionStr.str(), value);
			if (sendCommand(request, responses))
			{
				if (responses.size() > 0) return responses.at(0).IsOKFrame();
			}
		}
		else {
			std::vector<bt_openwebnet> responses;
			bt_openwebnet request(who, what, (where & ~OPENWEBNET_GROUP_ID), (where & OPENWEBNET_GROUP_ID));
			if (sendCommand(request, responses))
			{
				if (responses.size() > 0) return responses.at(0).IsOKFrame();
			}
		}
	}
	else
	{
		std::vector<bt_openwebnet> responses;
		bt_openwebnet request;

		std::stringstream whoStr;
		whoStr << who;
		std::string sWho = whoStr.str();

		std::stringstream whatStr;
		whatStr << what;
		std::string sWhat = whatStr.str();

		std::stringstream whereStr;
		whereStr << (where & ~OPENWEBNET_GROUP_ID);
		std::string sWhere = "";
		if ((where & OPENWEBNET_GROUP_ID))
			sWhere += "#";
		sWhere += whereStr.str();

		std::stringstream interfaceStr;
		interfaceStr << iInterface;
		std::string sInterface = interfaceStr.str();

		std::string lev = "";
		std::string when = "";
		request.CreateMsgOpen(sWho, sWhat, sWhere, lev, sInterface, when);
		if (sendCommand(request, responses))
		{
			if (responses.size() > 0) return responses.at(0).IsOKFrame();
		}
	}

	return true;
}


bool COpenWebNetTCP::SetSetpoint(const int idx, const float temp)
{
	//int who = (idx >> 16) & 0xff;  fixed to 'WHO_TEMPERATURE_CONTROL'
	int iInterface = (idx >> 8) & 0xff;
	int where = idx & 0xff;
	int _temp = (int)(temp * 10);

	std::vector<bt_openwebnet> responses;
	bt_openwebnet request;

	std::stringstream whoStr;
	whoStr << WHO_TEMPERATURE_CONTROL;
	std::string sWho = whoStr.str();

	std::stringstream whereStr;
	whereStr << (where & ~OPENWEBNET_GROUP_ID);
	std::string sWhere = "";
	// add # to set value permanent
	sWhere += "#" + whereStr.str();

	std::stringstream dimensionStr;
	dimensionStr << TEMPERATURE_CONTROL_DIMENSION_SET_POINT_TEMPERATURE;
	std::string sDimension = dimensionStr.str();

	std::stringstream valueStr;

	valueStr << 0;
	valueStr << _temp;
	std::vector<std::string> sValue;
	sValue.push_back(valueStr.str());
	sValue.push_back("3");					//send generic mode. We don't need to know in witch state the bt3550 or BTI-L4695 is (cooling or heating).

	if (iInterface)
	{
		// if Bus Id interface is present...
		std::stringstream interfaceStr;
		interfaceStr << iInterface;
		std::string sInterface = interfaceStr.str();

		std::string sLev = "";
		request.CreateWrDimensionMsgOpen(sWho, sWhere, sLev, sInterface, sDimension, sValue);
		if (sendCommand(request, responses))
		{
			if (responses.size() > 0) return responses.at(0).IsOKFrame();
		}
	}
	else
	{
		// Local Bus..
		request.CreateWrDimensionMsgOpen(sWho, sWhere, sDimension, sValue); // (const std::string& who, const std::string& where, const std::string& dimension, const std::vector<std::string>& value)
		if (sendCommand(request, responses, 1, false))
		{
			if (responses.size() > 0)
			{
				return responses.at(0).IsOKFrame();
			}
		}
	}

	return false;
}



/**
   Send OpenWebNet command to device
**/
bool COpenWebNetTCP::sendCommand(bt_openwebnet& command, std::vector<bt_openwebnet>& response, int waitForResponse, bool silent)
{
	csocket *commandSocket;
	bool ret;
	if (!(commandSocket = connectGwOwn(OPENWEBNET_COMMAND_SESSION)))
	{
		_log.Log(LOG_ERROR, "COpenWebNetTCP: Command session ERROR");
		return false;
	}
	// Command session correctly open
	_log.Log(LOG_STATUS, "COpenWebNetTCP: Command session connected to: %s:%d", m_szIPAddress.c_str(), m_usIPPort);

	// Command session correctly open -> write command
	int bytesWritten = commandSocket->write(command.frame_open.c_str(), command.frame_open.length());
	if (bytesWritten != command.frame_open.length()) {
		if (!silent) {
			_log.Log(LOG_ERROR, "COpenWebNetTCP sendCommand: partial write");
		}
	}

	if (waitForResponse > 0) {
		sleep_seconds(waitForResponse);

		char responseBuffer[OPENWEBNET_BUFFER_SIZE];
		memset(responseBuffer, 0, OPENWEBNET_BUFFER_SIZE);
		int read = commandSocket->read(responseBuffer, OPENWEBNET_BUFFER_SIZE, false);

		if (!silent) {
			_log.Log(LOG_STATUS, "COpenWebNetTCP: sent=%s received=%s", command.frame_open.c_str(), responseBuffer);
		}

		std::lock_guard<std::mutex> l(readQueueMutex);
		ret = ParseData(responseBuffer, read, response);
	}
	else
		ret = true;

	if (commandSocket->getState() != csocket::CLOSED)
		commandSocket->close();
	return (ret);
}

/**
	automatic scan of automation/lighting device
**/
void COpenWebNetTCP::scan_automation_lighting(const int cen_area)
{
	bt_openwebnet request;
	std::vector<bt_openwebnet> responses;
	std::stringstream whoStr;
	std::stringstream whereStr;
	whoStr << WHO_LIGHTING;
	whereStr << cen_area;
	request.CreateStateMsgOpen(whoStr.str(), whereStr.str());
	sendCommand(request, responses, 0, false);
}

/**
	automatic scan of temperature control device
**/
void COpenWebNetTCP::scan_temperature_control()
{
	bt_openwebnet request;
	std::vector<bt_openwebnet> responses;
	std::stringstream whoStr;
	std::stringstream dimensionStr;
	whoStr << WHO_TEMPERATURE_CONTROL;
	dimensionStr << 0;

	for (int where = 1; where < 100; where++)
	{
		std::stringstream whereStr;
		whereStr << where;
		request.CreateDimensionMsgOpen(whoStr.str(), whereStr.str(), dimensionStr.str());
		sendCommand(request, responses, 0, true);
	}
}

/**
	request general burglar alarm status
**/
void COpenWebNetTCP::requestBurglarAlarmStatus()
{
	bt_openwebnet request;
	std::vector<bt_openwebnet> responses;
	std::stringstream whoStr;
	std::stringstream whereStr;
	whoStr << WHO_BURGLAR_ALARM;
	whereStr << 0;
	request.CreateStateMsgOpen(whoStr.str(), whereStr.str());
	sendCommand(request, responses, 0, false);
}

/**
	request Dry contact/IR Detection alarm status
**/
void COpenWebNetTCP::requestDryContactIRDetectionStatus()
{
	bt_openwebnet request;
	std::vector<bt_openwebnet> responses;
	std::stringstream whoStr;
	std::stringstream whereStr;
	whoStr << WHO_CEN_PLUS_DRY_CONTACT_IR_DETECTION;
	whereStr << 30;
	request.CreateStateMsgOpen(whoStr.str(), whereStr.str());
	sendCommand(request, responses, 0, false);
}

/**
	request energy totalizer
**/
void COpenWebNetTCP::requestEnergyTotalizer()
{
	bt_openwebnet request;
	std::vector<bt_openwebnet> responses;
	std::stringstream whoStr;
	std::stringstream dimensionStr;
	whoStr << WHO_ENERGY_MANAGEMENT;
	dimensionStr << ENERGY_MANAGEMENT_DIMENSION_ENERGY_TOTALIZER;

	for (int where = WHERE_ENERGY_1; where < MAX_WHERE_ENERGY; where++)
	{
		std::stringstream whereStr;
		whereStr << where;
		request.CreateDimensionMsgOpen(whoStr.str(), whereStr.str(), dimensionStr.str());
		sendCommand(request, responses, 0, true);
	}
}

/**
	request automatic update power
**/
void COpenWebNetTCP::requestAutomaticUpdatePower(int time)
{
	bt_openwebnet request;
	std::vector<bt_openwebnet> responses;
	std::stringstream whoStr, dimensionStr, appStr;
	std::vector<std::string> value;
	whoStr << WHO_ENERGY_MANAGEMENT;
	dimensionStr << ENERGY_MANAGEMENT_DIMENSION_END_AUTOMATIC_UPDATE;

	if (time > 255) time = 255; // Time in minutes

	value.clear();
	value.push_back("1");
	appStr << time;
	value.push_back(appStr.str());

	for (int where = WHERE_ENERGY_1; where < MAX_WHERE_ENERGY; where++)
	{
		std::stringstream whereStr;
		whereStr << where;
		request.CreateWrDimensionMsgOpen2(whoStr.str(), whereStr.str(), dimensionStr.str(), value);
		sendCommand(request, responses, 0, true);
	}
}

/**
	Request time to gateway
**/
void COpenWebNetTCP::requestTime()
{
	_log.Log(LOG_STATUS, "COpenWebNetTCP: request time...");
	bt_openwebnet request;
	std::vector<bt_openwebnet> responses;
	request.CreateTimeReqMsgOpen();
	sendCommand(request, responses, 0, true);
}

void COpenWebNetTCP::setTime()
{
	_log.Log(LOG_STATUS, "COpenWebNetTCP: set DateTime...");
	bt_openwebnet request;
	std::vector<bt_openwebnet> responses;
	request.CreateSetTimeMsgOpen();
	sendCommand(request, responses, 0, true);
}

/**
	scan devices
**/
void COpenWebNetTCP::scan_device()
{
	int iWhere;

	/* uncomment the line below to enable the time request to the gateway.
	Note that this is only for debugging, the answer to who = 13 is not yet supported */
	//requestTime();

	/* uncomment the line below to enable the set time commenad to the gateway.*/
	//setTime();

	if (mask_request_status & 0x1)
	{
		_log.Log(LOG_STATUS, "COpenWebNetTCP: scanning automation/lighting...");
		// Scan of all devices
		scan_automation_lighting(WHERE_CEN_0);

		requestDryContactIRDetectionStatus();

		_log.Log(LOG_STATUS, "COpenWebNetTCP: request burglar alarm status...");
		requestBurglarAlarmStatus();

		/** Scanning of temperature sensor is not necessary simply wait an update **/
		//_log.Log(LOG_STATUS, "COpenWebNetTCP: scanning temperature control...");
		//scan_temperature_control();

		_log.Log(LOG_STATUS, "COpenWebNetTCP: scan device complete, wait all the update data..");

		/* Update complete scan time*/
		LastScanTime = mytime(NULL);
	}
	else
	{
		// Scan only the set areas
		for (iWhere = WHERE_AREA_1; iWhere < MAX_WHERE_AREA; iWhere++)
		{
			if (mask_request_status & (0x1 << iWhere))
			{
				_log.Log(LOG_STATUS, "COpenWebNetTCP: scanning AREA %u...", iWhere);
				scan_automation_lighting(iWhere);
			}
		}
	}
}

bool COpenWebNetTCP::ParseData(char* data, int length, std::vector<bt_openwebnet>& messages)
{
	std::string buffer = std::string(data, length);
	size_t begin = 0;
	size_t end = std::string::npos;
	do {
		end = buffer.find(OPENWEBNET_END_FRAME, begin);
		if (end != std::string::npos) {
			bt_openwebnet message(buffer.substr(begin, end - begin + 2));
			messages.push_back(message);
			begin = end + 2;
		}
	} while (end != std::string::npos);

	return true;
}

void COpenWebNetTCP::Do_Work()
{
	while (!IsStopRequested(OPENWEBNET_HEARTBEAT_DELAY*1000))
	{
		if (isStatusSocketConnected() && mask_request_status)
		{
			scan_device();
			mask_request_status = 0x0; // scan devices complete
		}

		// every 5 minuts force scan ALL devices for refresh status
		if (m_ownScanTime && ((mytime(NULL) - LastScanTime) > m_ownScanTime))
		{
			if ((mask_request_status & 0x1) == 0)
				_log.Log(LOG_STATUS, "COpenWebNetTCP: HEARTBEAT set scan devices ...");
			mask_request_status = 0x1; // force scan devices
		}

		if ((mytime(NULL) - LastScanTimeEnergy) > SCAN_TIME_REQ_AUTO_UPDATE_POWER)
		{
			requestAutomaticUpdatePower(255); // automatic update for 255 minutes
			LastScanTimeEnergy = mytime(NULL);
		}

		if ((mytime(NULL) - LastScanTimeEnergyTot) > SCAN_TIME_REQ_ENERGY_TOTALIZER)
		{
			requestEnergyTotalizer();
			LastScanTimeEnergyTot = mytime(NULL);
		}

		m_LastHeartbeat = mytime(NULL);
	}
	_log.Log(LOG_STATUS, "COpenWebNetTCP: Heartbeat worker stopped...");
}

/**
   Find OpenWebNetDevice in DB
**/
bool COpenWebNetTCP::FindDevice(const int who, const int where, const int iInterface, int* used)
{
	std::vector<std::vector<std::string> > result;
	int devType = -1;

	//make device ID
	int NodeID = (((int)who << 16) & 0xFFFF0000) | (((int)where) & 0x0000FFFF);

	switch (who) {
	case WHO_LIGHTING:                              // 1
	case WHO_AUTOMATION:                            // 2
	case WHO_AUXILIARY:                             // 9
	case WHO_CEN_PLUS_DRY_CONTACT_IR_DETECTION:     // 25
		//devType = pTypeGeneralSwitch;
		devType = pTypeLighting2;
		break;
	case WHO_TEMPERATURE_CONTROL:                   // 4
		return true; // device always present
	case WHO_SCENARIO:                              // 0
	case WHO_LOAD_CONTROL:                          // 3
	case WHO_BURGLAR_ALARM:                         // 5
	case WHO_DOOR_ENTRY_SYSTEM:                     // 6
	case WHO_MULTIMEDIA:                            // 7
	case WHO_GATEWAY_INTERFACES_MANAGEMENT:         // 13
	case WHO_LIGHT_SHUTTER_ACTUATOR_LOCK:           // 14
	case WHO_SCENARIO_SCHEDULER_SWITCH:             // 15
	case WHO_AUDIO:                                 // 16
	case WHO_SCENARIO_PROGRAMMING:                  // 17
	case WHO_ENERGY_MANAGEMENT:                     // 18
	case WHO_LIHGTING_MANAGEMENT:                   // 24
	case WHO_ZIGBEE_DIAGNOSTIC:                     // 1000
	case WHO_AUTOMATIC_DIAGNOSTIC:                  // 1001
	case WHO_THERMOREGULATION_DIAGNOSTIC_FAILURES:  // 1004
	case WHO_DEVICE_DIAGNOSTIC:                     // 1013
	default:
		return false;
	}

	if (devType >= 0)
	{
		char szIdx[10];
		sprintf(szIdx, "%07X", NodeID);

		if (used != NULL)
		{
			result = m_sql.safe_query("SELECT ID FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q') AND (Unit == %d) AND (Type==%d) AND Used == %d",
				m_HwdID, szIdx, iInterface, devType, *used);
		}
		else
		{
			result = m_sql.safe_query("SELECT ID FROM DeviceStatus WHERE (HardwareID==%d) AND (DeviceID=='%q') AND (Unit == %d) AND (Type==%d)",
				m_HwdID, szIdx, iInterface, devType);
		}
	}
	else
		return false;


	if (!result.empty())
	{
		return true;
	}

	return false;
}
