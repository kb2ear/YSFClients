/*
*   Copyright (C) 2016,2017,2018 by Jonathan Naylor G4KLX
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "YSFGateway.h"
#include "YSFReflectors.h"
#include "UDPSocket.h"
#include "StopWatch.h"
#include "Version.h"
#include "YSFFICH.h"
#include "Thread.h"
#include "Timer.h"
#include "Log.h"

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "YSFGateway.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/YSFGateway.ini";
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>

int main(int argc, char** argv)
{
	const char* iniFile = DEFAULT_INI_FILE;
	if (argc > 1) {
		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "YSFGateway version %s\n", VERSION);
				return 0;
			} else if (arg.substr(0, 1) == "-") {
				::fprintf(stderr, "Usage: YSFGateway [-v|--version] [filename]\n");
				return 1;
			} else {
				iniFile = argv[currentArg];
			}
		}
	}

	CYSFGateway* gateway = new CYSFGateway(std::string(iniFile));

	int ret = gateway->run();

	delete gateway;

	return ret;
}

CYSFGateway::CYSFGateway(const std::string& configFile) :
m_callsign(),
m_suffix(),
m_conf(configFile),
m_gps(NULL),
m_wiresX(NULL),
m_dtmf(),
m_ysfNetwork(NULL),
m_fcsNetwork(NULL),
m_linkType(LINK_NONE),
m_exclude(false),
m_inactivityTimer(1000U),
m_lostTimer(1000U, 120U),
m_ysfPollTimer(1000U, 5U)
{
}

CYSFGateway::~CYSFGateway()
{
}

int CYSFGateway::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "YSFGateway: cannot read the .ini file\n");
		return 1;
	}

	setlocale(LC_ALL, "C");

	ret = ::LogInitialise(m_conf.getLogFilePath(), m_conf.getLogFileRoot(), m_conf.getLogFileLevel(), m_conf.getLogDisplayLevel());
	if (!ret) {
		::fprintf(stderr, "YSFGateway: unable to open the log file\n");
		return 1;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::LogWarning("Couldn't fork() , exiting");
			return -1;
		} else if (pid != 0)
			exit(EXIT_SUCCESS);

		// Create new session and process group
		if (::setsid() == -1) {
			::LogWarning("Couldn't setsid(), exiting");
			return -1;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1) {
			::LogWarning("Couldn't cd /, exiting");
			return -1;
		}

		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);

		//If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == NULL) {
				::LogError("Could not get the mmdvm user, exiting");
				return -1;
			}

			uid_t mmdvm_uid = user->pw_uid;
			gid_t mmdvm_gid = user->pw_gid;

			//Set user and group ID's to mmdvm:mmdvm
			if (setgid(mmdvm_gid) != 0) {
				::LogWarning("Could not set mmdvm GID, exiting");
				return -1;
			}

			if (setuid(mmdvm_uid) != 0) {
				::LogWarning("Could not set mmdvm UID, exiting");
				return -1;
			}

			//Double check it worked (AKA Paranoia) 
			if (setuid(0) != -1) {
				::LogWarning("It's possible to regain root - something is wrong!, exiting");
				return -1;
			}
		}
	}
#endif

	m_callsign = m_conf.getCallsign();
	m_suffix   = m_conf.getSuffix();

	bool debug            = m_conf.getYSFNetworkDebug();
	in_addr rptAddress    = CUDPSocket::lookup(m_conf.getRptAddress());
	unsigned int rptPort  = m_conf.getRptPort();
	std::string myAddress = m_conf.getMyAddress();
	unsigned int myPort   = m_conf.getMyPort();

	CYSFNetwork rptNetwork(myAddress, myPort, m_callsign, debug);
	rptNetwork.setDestination(rptAddress, rptPort);

	ret = rptNetwork.open();
	if (!ret) {
		::LogError("Cannot open the repeater network port");
		::LogFinalise();
		return 1;
	}

	unsigned int ysfPort = m_conf.getYSFNetworkPort();

	m_ysfNetwork = new CYSFNetwork(ysfPort, m_callsign, debug);
	ret = m_ysfNetwork->open();
	if (!ret) {
		::LogError("Cannot open the YSF reflector network port");
		::LogFinalise();
		return 1;
	}

	unsigned int txFrequency = m_conf.getTxFrequency();
	unsigned int rxFrequency = m_conf.getRxFrequency();
	std::string locator      = m_conf.getLocator();
	unsigned int id          = m_conf.getId();

	unsigned int fcsPort = m_conf.getFCSNetworkPort();

	m_fcsNetwork = new CFCSNetwork(fcsPort, m_callsign, rxFrequency, txFrequency, locator, id, debug);
	ret = m_fcsNetwork->open();
	if (!ret) {
		::LogError("Cannot open the FCS reflector network port");
		::LogFinalise();
		return 1;
	}

	m_inactivityTimer.setTimeout(m_conf.getYSFNetworkInactivityTimeout() * 60U);

	bool revert = m_conf.getYSFNetworkRevert();
	std::string startup = m_conf.getYSFNetworkStartup();

	bool fcsNetworkEnabled = m_conf.getFCSNetworkEnabled();
	bool ysfNetworkEnabled = m_conf.getYSFNetworkEnabled();
	if (ysfNetworkEnabled) {
		std::string fileName    = m_conf.getYSFNetworkHosts();
		unsigned int reloadTime = m_conf.getYSFNetworkReloadTime();

		m_wiresX = new CWiresX(m_callsign, m_suffix, &rptNetwork, fileName, reloadTime);

		std::string name         = m_conf.getName();

		m_wiresX->setInfo(name, txFrequency, rxFrequency);

		std::string address = m_conf.getYSFNetworkParrotAddress();
		unsigned int port = m_conf.getYSFNetworkParrotPort();

		if (port > 0U)
			m_wiresX->setParrot(address, port);

		address = m_conf.getYSFNetworkYSF2DMRAddress();
		port = m_conf.getYSFNetworkYSF2DMRPort();

		if (port > 0U)
			m_wiresX->setYSF2DMR(address, port);

		m_wiresX->start();

		if (!startup.empty()) {
			CYSFReflector* reflector = m_wiresX->getReflector(startup);
			if (reflector != NULL) {
				LogMessage("Automatic connection to %5.5s - \"%s\"", reflector->m_id.c_str(), reflector->m_name.c_str());

				m_ysfNetwork->setDestination(reflector->m_address, reflector->m_port);
				m_ysfNetwork->writePoll();
				m_ysfNetwork->writePoll();
				m_ysfNetwork->writePoll();

				if (!revert)
					m_inactivityTimer.start();

				m_lostTimer.start();
				m_ysfPollTimer.start();

				m_linkType = LINK_YSF;
			}
		}
	}

	CStopWatch stopWatch;
	stopWatch.start();

	LogMessage("Starting YSFGateway-%s", VERSION);

	createGPS();

	for (;;) {
		unsigned char buffer[200U];

		while (rptNetwork.read(buffer) > 0U) {
			CYSFFICH fich;
			bool valid = fich.decode(buffer + 35U);
			if (valid) {
				unsigned char fi = fich.getFI();
				unsigned char dt = fich.getDT();
				unsigned char fn = fich.getFN();
				unsigned char ft = fich.getFT();

				// Don't send out control data
				m_exclude = (dt == YSF_DT_DATA_FR_MODE);

				if (m_wiresX != NULL)
					processWiresX(buffer, fi, dt, fn, ft);

				processDTMF(buffer, dt);

				if (m_gps != NULL)
					m_gps->data(buffer + 14U, buffer + 35U, fi, dt, fn, ft);
			}

			if (ysfNetworkEnabled && m_linkType == LINK_YSF && !m_exclude) {
				m_ysfNetwork->write(buffer);
				if (::memcmp(buffer + 0U, "YSFD", 4U) == 0)
					m_inactivityTimer.start();
			}

			if (fcsNetworkEnabled && m_linkType == LINK_FCS && !m_exclude) {
				if (::memcmp(buffer + 0U, "YSFD", 4U) == 0) {
					m_fcsNetwork->write(buffer);
					m_inactivityTimer.start();
				}
			}

			if ((buffer[34U] & 0x01U) == 0x01U) {
				if (m_gps != NULL)
					m_gps->reset();
				m_dtmf.reset();
				m_exclude = false;
			}
		}

		while (m_ysfNetwork->read(buffer) > 0U) {
			if (ysfNetworkEnabled && m_linkType == LINK_YSF) {
				// Only pass through YSF data packets
				if (::memcmp(buffer + 0U, "YSFD", 4U) == 0)
					rptNetwork.write(buffer);

				m_lostTimer.start();
			}
		}

		while (m_fcsNetwork->read(buffer) > 0U) {
			if (fcsNetworkEnabled && m_linkType == LINK_FCS) {
				rptNetwork.write(buffer);
				m_lostTimer.start();
			}
		}

		unsigned int ms = stopWatch.elapsed();
		stopWatch.start();

		rptNetwork.clock(ms);
		m_ysfNetwork->clock(ms);
		m_fcsNetwork->clock(ms);
		if (m_gps != NULL)
			m_gps->clock(ms);
		if (m_wiresX != NULL)
			m_wiresX->clock(ms);

		m_inactivityTimer.clock(ms);
		if (m_inactivityTimer.isRunning() && m_inactivityTimer.hasExpired()) {
			if (m_linkType == LINK_YSF) {
				CYSFReflector* reflector = NULL;
				if (revert && !startup.empty() && m_wiresX != NULL)
					reflector = m_wiresX->getReflector(startup);

				if (reflector != NULL) {
					LogMessage("Reverting connection to %5.5s - \"%s\"", reflector->m_id.c_str(), reflector->m_name.c_str());

					if (m_wiresX != NULL)
						m_wiresX->processConnect(reflector);

					m_ysfNetwork->writeUnlink();
					m_ysfNetwork->writeUnlink();
					m_ysfNetwork->writeUnlink();

					m_ysfNetwork->setDestination(reflector->m_address, reflector->m_port);
					m_ysfNetwork->writePoll();
					m_ysfNetwork->writePoll();
					m_ysfNetwork->writePoll();

					m_lostTimer.start();
					m_ysfPollTimer.start();
				} else {
					LogMessage("Disconnecting due to inactivity");

					if (m_wiresX != NULL)
						m_wiresX->processDisconnect();

					m_ysfNetwork->writeUnlink();
					m_ysfNetwork->writeUnlink();
					m_ysfNetwork->writeUnlink();
					m_ysfNetwork->clearDestination();

					m_lostTimer.stop();
					m_ysfPollTimer.stop();

					m_linkType = LINK_NONE;
				}
			}

			m_inactivityTimer.stop();
		}

		m_lostTimer.clock(ms);
		if (m_lostTimer.isRunning() && m_lostTimer.hasExpired()) {
			LogWarning("Link has failed, polls lost");

			if (m_wiresX != NULL)
				m_wiresX->processDisconnect();

			if (m_fcsNetwork != NULL)
				m_fcsNetwork->clearDestination();

			if (m_ysfNetwork != NULL)
				m_ysfNetwork->clearDestination();

			m_inactivityTimer.stop();
			m_lostTimer.stop();
			m_ysfPollTimer.stop();

			m_linkType = LINK_NONE;
		}

		m_ysfPollTimer.clock(ms);
		if (m_ysfPollTimer.isRunning() && m_ysfPollTimer.hasExpired()) {
			m_ysfNetwork->writePoll();
			m_ysfPollTimer.start();
		}

		if (ms < 5U)
			CThread::sleep(5U);
	}

	rptNetwork.close();
	m_ysfNetwork->close();
	m_fcsNetwork->close();

	if (m_gps != NULL) {
		m_gps->close();
		delete m_gps;
	}

	delete m_ysfNetwork;
	delete m_fcsNetwork;
	delete m_wiresX;

	::LogFinalise();

	return 0;
}

void CYSFGateway::createGPS()
{
	if (!m_conf.getAPRSEnabled())
		return;

	std::string hostname = m_conf.getAPRSServer();
	unsigned int port    = m_conf.getAPRSPort();
	std::string password = m_conf.getAPRSPassword();
	std::string desc     = m_conf.getAPRSDescription();

	m_gps = new CGPS(m_callsign, m_suffix, password, hostname, port);

	unsigned int txFrequency = m_conf.getTxFrequency();
	unsigned int rxFrequency = m_conf.getRxFrequency();
	float latitude           = m_conf.getLatitude();
	float longitude          = m_conf.getLongitude();
	int height               = m_conf.getHeight();

	m_gps->setInfo(txFrequency, rxFrequency, latitude, longitude, height, desc);

	bool ret = m_gps->open();
	if (!ret) {
		delete m_gps;
		m_gps = NULL;
	}
}

void CYSFGateway::processWiresX(const unsigned char* buffer, unsigned char fi, unsigned char dt, unsigned char fn, unsigned char ft)
{
	assert(buffer != NULL);

	WX_STATUS status = m_wiresX->process(buffer + 35U, buffer + 14U, fi, dt, fn, ft);
	switch (status) {
	case WXS_CONNECT_YSF: {
			if (m_linkType == LINK_YSF) {
				m_ysfNetwork->writeUnlink();
				m_ysfNetwork->writeUnlink();
				m_ysfNetwork->writeUnlink();
			}
			if (m_linkType == LINK_FCS) {
				m_fcsNetwork->writeUnlink();
				m_fcsNetwork->writeUnlink();
				m_fcsNetwork->writeUnlink();
				m_fcsNetwork->clearDestination();
			}

			CYSFReflector* reflector = m_wiresX->getReflector();
			LogMessage("Connect to %5.5s - \"%s\" has been requested by %10.10s", reflector->m_id.c_str(), reflector->m_name.c_str(), buffer + 14U);

			m_ysfNetwork->setDestination(reflector->m_address, reflector->m_port);
			m_ysfNetwork->writePoll();
			m_ysfNetwork->writePoll();
			m_ysfNetwork->writePoll();

			m_inactivityTimer.start();
			m_lostTimer.start();
			m_ysfPollTimer.start();

			m_linkType = LINK_YSF;
		}
		break;
	case WXS_DISCONNECT:
		if (m_linkType == LINK_YSF) {
			LogMessage("Disconnect has been requested by %10.10s", buffer + 14U);

			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->clearDestination();

			m_inactivityTimer.stop();
			m_lostTimer.stop();
			m_ysfPollTimer.stop();

			m_linkType = LINK_NONE;
		}
		if (m_linkType == LINK_FCS) {
			LogMessage("Disconnect has been requested by %10.10s", buffer + 14U);

			m_fcsNetwork->writeUnlink();
			m_fcsNetwork->writeUnlink();
			m_fcsNetwork->writeUnlink();
			m_fcsNetwork->clearDestination();

			m_inactivityTimer.stop();
			m_lostTimer.stop();

			m_linkType = LINK_NONE;
		}
		break;
	default:
		break;
	}
}

void CYSFGateway::processDTMF(const unsigned char* buffer, unsigned char dt)
{
	assert(buffer != NULL);

	WX_STATUS status = WXS_NONE;
	switch (dt) {
	case YSF_DT_VD_MODE2:
		status = m_dtmf.decodeVDMode2(buffer + 35U, (buffer[34U] & 0x01U) == 0x01U);
		break;
	default:
		break;
	}

	switch (status) {
	case WXS_CONNECT_YSF: {
			std::string id = m_dtmf.getReflector();
			CYSFReflector* reflector = m_wiresX->getReflector(id);
			if (reflector != NULL) {
				if (m_wiresX != NULL)
					m_wiresX->processConnect(reflector);

				if (m_linkType == LINK_YSF) {
					m_ysfNetwork->writeUnlink();
					m_ysfNetwork->writeUnlink();
					m_ysfNetwork->writeUnlink();
				}
				if (m_linkType == LINK_FCS) {
					m_fcsNetwork->writeUnlink();
					m_fcsNetwork->writeUnlink();
					m_fcsNetwork->writeUnlink();
					m_fcsNetwork->clearDestination();
				}

				LogMessage("Connect via DTMF to %5.5s - \"%s\" has been requested by %10.10s", reflector->m_id.c_str(), reflector->m_name.c_str(), buffer + 14U);

				m_ysfNetwork->setDestination(reflector->m_address, reflector->m_port);
				m_ysfNetwork->writePoll();
				m_ysfNetwork->writePoll();
				m_ysfNetwork->writePoll();

				m_inactivityTimer.start();
				m_lostTimer.start();
				m_ysfPollTimer.start();

				m_linkType = LINK_YSF;
			}
		}
		break;
	case WXS_CONNECT_FCS: {
		std::string id = m_dtmf.getReflector();
		if (id.length() == 2U)
			id = "FCS00" + id.at(0U) + std::string("0") + id.at(1U);
		else
			id = "FCS00" + id.at(0U) + id.at(1U) + id.at(2U);

		if (m_linkType == LINK_YSF) {
			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->clearDestination();
			m_ysfPollTimer.stop();
		}
		if (m_linkType == LINK_FCS) {
			m_fcsNetwork->writeUnlink();
			m_fcsNetwork->writeUnlink();
			m_fcsNetwork->writeUnlink();
		}

		LogMessage("Connect via DTMF to %s has been requested by %10.10s", id.c_str(), buffer + 14U);

		m_fcsNetwork->writeLink(id);
		m_fcsNetwork->writeLink(id);
		m_fcsNetwork->writeLink(id);

		m_inactivityTimer.start();
		m_lostTimer.start();

		m_linkType = LINK_FCS;
	}
    break;
	case WXS_DISCONNECT:
		if (m_linkType == LINK_YSF) {
			if (m_wiresX != NULL)
				m_wiresX->processDisconnect();

			LogMessage("Disconnect via DTMF has been requested by %10.10s", buffer + 14U);

			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->writeUnlink();
			m_ysfNetwork->clearDestination();

			m_inactivityTimer.stop();
			m_lostTimer.stop();
			m_ysfPollTimer.stop();

			m_linkType = LINK_NONE;
		}
		if (m_linkType == LINK_FCS) {
			LogMessage("Disconnect via DTMF has been requested by %10.10s", buffer + 14U);

			m_fcsNetwork->writeUnlink();
			m_fcsNetwork->writeUnlink();
			m_fcsNetwork->writeUnlink();
			m_fcsNetwork->clearDestination();

			m_inactivityTimer.stop();
			m_lostTimer.stop();

			m_linkType = LINK_NONE;
		}
		break;
	default:
		break;
	}
}
