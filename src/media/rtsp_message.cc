
/******************************************************
*   Copyright (C)2019 All rights reserved.
*
*   Author        : owb
*   Email         : 2478644416@qq.com
*   File Name     : rtsp_message.cc
*   Last Modified : 2020-12-11 19:44
*   Describe      :
*
*******************************************************/

#include "rtsp_message.h"

#include "muduo/base/Logging.h"
#include "muduo/net/Buffer.h"

#include <utility>

bool RtspRequest::parseRequest(muduo::net::Buffer* buffer) {
    LOG_INFO << "RtspRequest::parseRequest";
    if(buffer->peek()[0] == '$') {
        _method = RTCP;
        return true;
    }

    bool ret = true;
    while(1) {
        if(_state == kParseRequestLine) {
            const char* firstCrlf = buffer->findCRLF();
            if(firstCrlf) {
                ret = parseRequestLine(buffer->peek(), firstCrlf);
                buffer->retrieveUntil(firstCrlf+2);
            }

            if(_state == kParseHeadersLine)
                continue;
            else
                break;
        }
        else if(_state == kParseHeadersLine) {
            const char* lastCrlf = buffer->findLastCRLF();
            if(lastCrlf) {
                ret = parseHeadersLine(buffer->peek(), lastCrlf);
                buffer->retrieveUntil(lastCrlf+2);
            }
            break;
        }
        else if(_state == kGotAll) {
            buffer->retrieveAll();
            return true;
        }
    }
    return ret;
}

bool RtspRequest::parseRequestLine(const char* begin, const char* end) {
    LOG_INFO << "RtspRequest::parseRequestLine";
    std::string message(begin, end);
    char method[32] = { 0 };
    char url[128] = { 0 };
    char version[16] = { 0 };

    if(sscanf(message.c_str(), "%s %s %s", method, url, version) != 3) {
        return false;
    }

    std::string methodString(method);
	if(methodString == "OPTIONS") {
        _method = OPTIONS;
    }
    else if(methodString == "DESCRIBE") {
        _method = DESCRIBE;
    }
    else if(methodString == "SETUP") {
        _method = SETUP;
    }
    else if(methodString == "PLAY") {
        _method = PLAY;
    }
    else if(methodString == "TEARDOWN") {
        _method = TEARDOWN;
    }
    else if(methodString == "GET_PARAMETER") {
        _method = GET_PARAMETER;
    }
    else {
        _method = NONE;
        return false;
    }    

	if(strncmp(url, "rtsp://", 7) != 0)
		return false;

	uint16_t port = 0;
	char ip[16] = { 0 };
    char suffix[64] = { 0 };

    if(sscanf(url+7, "%[^:]:%hu/%s", ip, &port, suffix) == 3) {
    }
    else if(sscanf(url+7, "%[^/]/%s", ip, suffix) == 2) {
        port = 554;
    }
    else {
        return false;
    }

   	_requestLineParam.emplace("url", std::make_pair(std::string(url), 0));
    _requestLineParam.emplace("url_ip", std::make_pair(std::string(ip), 0));
    _requestLineParam.emplace("url_port", std::make_pair("", (uint32_t)port));
    _requestLineParam.emplace("url_suffix", std::make_pair(std::string(suffix), 0));
    _requestLineParam.emplace("version", std::make_pair(std::string(version), 0));
    _requestLineParam.emplace("method", std::make_pair(std::string(methodString), 0));

    LOG_INFO << url << ", " << ip << ", " << port << ", "
             << suffix << ", " << version << ", " << methodString;

	_state = kParseHeadersLine;
	return true;
}

bool RtspRequest::parseHeadersLine(const char* begin, const char* end) {
    std::string message(begin, end);
    if(!parseCSeq(message)) {
        if(_headerLineParam.find("cseq") == _headerLineParam.end())
            return false;
    }

    if(_method == OPTIONS) {
        _state = kGotAll;
        return true;
    }

    if(_method == DESCRIBE) {
        if(parseAccept(message)) {
            _state = kGotAll;
        }
        return true;
    }

    if(_method == SETUP) {
        if(parseTransport(message)) {
            parseMediaChannel(message);
            _state = kGotAll;
        }

        return true;
    }

    if(_method == PLAY) {
        if(parseSessionId(message)) {
            _state = kGotAll;
        }
        return true;
    }

    if(_method == TEARDOWN) {
        _state = kGotAll;
        return true;
    }

    if(_method == GET_PARAMETER) {
        _state = kGotAll;
        return true;
    }

    return true;
}

bool RtspRequest::parseCSeq(std::string& message) {
    std::size_t pos = message.find("CSeq");
    if(pos != std::string::npos) {
        uint32_t cseq = 0;
        sscanf(message.c_str()+pos, "%*[^:]: %u", &cseq);
        _headerLineParam.emplace("cseq", std::make_pair("", cseq));
        return true;
    }
    return false;
}

bool RtspRequest::parseAccept(std::string& message) {
    if(message.rfind("Accept") == std::string::npos ||
        message.rfind("sdp") == std::string::npos)
        return false;
    return true;
}

bool RtspRequest::parseTransport(std::string& message) {
    std::size_t pos = message.find("Transport");
    if(pos != std::string::npos) {
        if((pos = message.find("RTP/AVP/TCP")) != std::string::npos) {
            _transport = RTP_OVER_TCP;
            uint16_t rtpChannel = 0, rtcpChannel = 0;
			if (sscanf(message.c_str() + pos, "%*[^;];%*[^;];%*[^=]=%hu-%hu", &rtpChannel, &rtcpChannel) != 2)
			{
				return false;
			}
            _headerLineParam.emplace("rtp_channel", std::make_pair("", rtpChannel));
            _headerLineParam.emplace("rtcp_channel", std::make_pair("", rtcpChannel));
        }
	    else if((pos = message.find("RTP/AVP")) != std::string::npos) {
            uint16_t rtpPort = 0, rtcpPort = 0;
            if(((message.find("unicast", pos)) != std::string::npos)) {
                _transport = RTP_OVER_UDP;
                if(sscanf(message.c_str()+pos, "%*[^;];%*[^;];%*[^=]=%hu-%hu",
                	&rtpPort, &rtcpPort) != 2) {
                    return false;
                }
            }
            else if((message.find("multicast", pos)) != std::string::npos) {
                _transport = RTP_OVER_MULTICAST;
            }
			else {
				return false;
			}

            _headerLineParam.emplace("rtp_port", std::make_pair("", rtpPort));
            _headerLineParam.emplace("rtcp_port", std::make_pair("", rtcpPort));
        }
		else {
            return false;
        }
        return true;
    }
    return false;
}

bool RtspRequest::parseSessionId(std::string& message) {
    std::size_t pos = message.find("Session");
    if (pos != std::string::npos) {
        uint32_t sessionId = 0;
        if(sscanf(message.c_str()+pos, "%*[^:]: %u", &sessionId) != 1)
            return false;
        return true;
    }
    return false;
}

bool RtspRequest::parseMediaChannel(std::string& message) {
    _channelId = channel_0;

    auto iter = _requestLineParam.find("url");
    if(iter != _requestLineParam.end()) {
        std::size_t pos = iter->second.first.find("track1");
        if(pos != std::string::npos)
            _channelId = channel_1;
    }
    return true;
}

uint32_t RtspRequest::getCSeq() const {
	uint32_t cseq = 0;
    auto iter = _headerLineParam.find("cseq");
    if(iter != _headerLineParam.end()) {
        cseq = iter->second.second;
    }
    return cseq;
}

std::string RtspRequest::getRtspUrl() const {
    auto iter = _requestLineParam.find("url");
    if(iter != _requestLineParam.end()) {
        return iter->second.first;
    }
    return "";
}

std::string RtspRequest::getRtspUrlSuffix() const {
    auto iter = _requestLineParam.find("url_suffix");
    if(iter != _requestLineParam.end()) {
        return iter->second.first;
    }
    return "";
}

std::string RtspRequest::getIp() const {
    auto iter = _requestLineParam.find("url_ip");
    if(iter != _requestLineParam.end()) {
        return iter->second.first;
    }
    return "";	
}

uint8_t RtspRequest::getRtpChannel() const {
    auto iter = _headerLineParam.find("rtp_channel");
    if(iter != _headerLineParam.end()) {
        return iter->second.second;
    }
    return 0;
}

uint8_t RtspRequest::getRtcpChannel() const {
    auto iter = _headerLineParam.find("rtcp_channel");
    if(iter != _headerLineParam.end()) {
        return iter->second.second;
    }
    return 0;
}

uint16_t RtspRequest::getRtpPort() const {
    auto iter = _headerLineParam.find("rtp_port");
    if(iter != _headerLineParam.end()) {
        return iter->second.second;
    }
    return 0;
}

uint16_t RtspRequest::getRtcpPort() const {
	auto iter = _headerLineParam.find("rtcp_port");
    if(iter != _headerLineParam.end()) {
        return iter->second.second;
    }
    return 0;
}

int RtspRequest::buildOptionRes(const char* buf, int bufSize) {
    memset((void*)buf, 0, sizeof(buf));
    snprintf((char*)buf, bufSize,
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %u\r\n"
             "Public: OPTIONS, DESCRIBE, SETUP, TRARDOWN, PLAT\r\n"
             "\r\n",
             this->getCSeq());
    return (int)strlen(buf);
}

int RtspRequest::buildDescribeRes(const char* buf, int bufSize, const char* strSdp) {
	memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %u\r\n"
            "Content-Length: %d\r\n"
            "Content-Type: application/sdp\r\n"
            "\r\n"
            "%s",
            this->getCSeq(), 
            (int)strlen(strSdp), 
            strSdp);
    return (int)strlen(buf);
}

int RtspRequest::buildSetupMulticastRes(const char* buf, int bufSize, const char* strMulticastIp, uint16_t port, uint32_t sessionId) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
           	 "RTSP/1.0 200 OK\r\n"
             "CSeq: %u\r\n"
             "Transport: RTP/AVP;multicast;destination=%s;source=%s;port=%u-0;ttl=255\r\n"
             "Session: %u\r\n"
             "\r\n",
             this->getCSeq(),
             strMulticastIp,
             this->getIp().c_str(),
             port,
             sessionId);
    return (int)strlen(buf);
}

int RtspRequest::buildSetupTcpRes(const char* buf, int bufSize, uint16_t rtpChn, uint16_t rtcpChn, uint32_t sessionId) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %u\r\n"
             "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
             "Session: %u\r\n"
             "\r\n",
             this->getCSeq(),
             rtpChn, rtcpChn,
             sessionId);
    return (int)strlen(buf);
}

int RtspRequest::buildSetupUdpRes(const char* buf, int bufSize, uint16_t rtpChn, uint16_t rtcpChn, uint32_t sessionId) {
	memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %u\r\n"
             "Transport: RTP/AVP;unicast;client_port=%hu-%hu;server_port=%hu-%hu\r\n"
             "Session: %u\r\n"
             "\r\n",
             this->getCSeq(),
             this->getRtpPort(),
             this->getRtcpPort(),
             rtpChn, 
             rtcpChn,
             sessionId);
    return (int)strlen(buf);
}
int RtspRequest::buildPlayRes(const char* buf, int bufSize, const char* rtpInfo, uint32_t sessionId) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Range: npt=0.000-\r\n"
            "Session: %u; timeout=60\r\n",
            this->getCSeq(),
            sessionId);
    if(rtpInfo != nullptr) {
        snprintf((char*)buf + strlen(buf), bufSize - strlen(buf), "%s\r\n", rtpInfo);
    }

    snprintf((char*)buf + strlen(buf), bufSize - strlen(buf), "\r\n");
    return (int)strlen(buf);
}

int RtspRequest::buildTeardownRes(const char* buf, int bufSize, uint32_t sessionId) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %u\r\n"
            "\r\n",
            this->getCSeq(),
            sessionId);
    return (int)strlen(buf);
}

int RtspRequest::buildGetParamterRes(const char* buf, int bufSize, uint32_t sessionId) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %u\r\n"
            "\r\n",
            this->getCSeq(),
            sessionId);
    return (int)strlen(buf);
}

int RtspRequest::buildNotFoundRes(const char* buf, int bufSize) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "RTSP/1.0 404 Stream Not Found\r\n"
            "CSeq: %u\r\n"
            "\r\n",
            this->getCSeq());
    return (int)strlen(buf);
}

int RtspRequest::buildServerErrorRes(const char* buf, int bufSize) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "RTSP/1.0 500 Internal Server Error\r\n"
            "CSeq: %u\r\n"
            "\r\n",
            this->getCSeq());
    return (int)strlen(buf);
}

int RtspRequest::buildUnsupportedRes(const char* buf, int bufSize) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "RTSP/1.0 461 Unsupported transport\r\n"
            "CSeq: %d\r\n"
            "\r\n",
            this->getCSeq());
    return (int)strlen(buf);
}


bool RtspResponse::parseResponse(muduo::net::Buffer* buffer) { // TODO
    if(strstr(buffer->peek(), "\r\n\r\n") != NULL) {
        if(strstr(buffer->peek(), "OK") == NULL) {
            return false;
        }

        const char* ptr = strstr(buffer->peek(), "Session");
        if(ptr) {
            char sessionId[50] = {0};
            if(sscanf(ptr, "%*[^:]: %s", sessionId) == 1)
            	_session = sessionId;
        }

        _cseq++;
        buffer->retrieveUntil("\r\n\r\n");
    }

    return true;
}

int RtspResponse::buildOptionReq(const char* buf, int bufSize) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "OPTIONS %s RTSP/1.0\r\n"
            "CSeq: %u\r\n"
            "User-Agent: %s\r\n"
            "\r\n",
            _rtspUrl.c_str(),
            this->getCSeq() + 1,
            _userAgent.c_str());
    _method = OPTIONS;
    return (int)strlen(buf);
}

int RtspResponse::buildDescribeReq(const char* buf, int bufSize) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "DESCRIBE %s RTSP/1.0\r\n"
            "CSeq: %u\r\n"
            "Accept: application/sdp\r\n"
            "User-Agent: %s\r\n"
            "\r\n",
            _rtspUrl.c_str(),
            this->getCSeq() + 1,
            _userAgent.c_str());
    _method = DESCRIBE;
    return (int)strlen(buf);
}

int RtspResponse::buildAnnounceReq(const char* buf, int bufSize, const char* strSdp) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "ANNOUNCE %s RTSP/1.0\r\n"
            "Content-Type: application/sdp\r\n"
            "CSeq: %u\r\n"
            "User-Agent: %s\r\n"
            "Session: %s\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            _rtspUrl.c_str(),
            this->getCSeq() + 1, 
            _userAgent.c_str(),
            this->getSession().c_str(),
            (int)strlen(strSdp),
            strSdp);
    _method = ANNOUNCE;
    return (int)strlen(buf);
}

int RtspResponse::buildSetupTcpReq(const char* buf, int bufSize, int trackId) {
    int interleaved[2] = { 0, 1 };
    if (trackId == 1) {
        interleaved[0] = 2;
        interleaved[1] = 3;
    }

    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "SETUP %s/track%d RTSP/1.0\r\n"
            "Transport: RTP/AVP/TCP;unicast;mode=record;interleaved=%d-%d\r\n"
            "CSeq: %u\r\n"
            "User-Agent: %s\r\n"
            "Session: %s\r\n"
            "\r\n",
            _rtspUrl.c_str(), 
            trackId,
            interleaved[0],
            interleaved[1],
            this->getCSeq() + 1,
            _userAgent.c_str(), 
            this->getSession().c_str());
    _method = SETUP;
    return (int)strlen(buf);
}

int RtspResponse::buildRecordReq(const char* buf, int bufSize) {
    memset((void*)buf, 0, bufSize);
    snprintf((char*)buf, bufSize,
            "RECORD %s RTSP/1.0\r\n"
            "Range: npt=0.000-\r\n"
            "CSeq: %u\r\n"
            "User-Agent: %s\r\n"
            "Session: %s\r\n"
            "\r\n",
            _rtspUrl.c_str(), 
            this->getCSeq() + 1,
            _userAgent.c_str(), 
            this->getSession().c_str());
    _method = RECORD;
    return (int)strlen(buf);
}

