/*
 *   Copyright (C) 2009-2014,2016,2017,2018 by Jonathan Naylor G4KLX
 *   Copyright (C) 2018 by Andy Uribe CA6JAU
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

#include "NXDNNetwork.h"
#include "Utils.h"
#include "Log.h"

#include <cstdio>
#include <cassert>
#include <cstring>

const unsigned int BUFFER_LENGTH = 200U;

// The NXDN voice frame period, matching the 12.5 frames/second used elsewhere
// in the converter. It is the rate at which the receive jitter buffer releases
// frames and repeats the last one when a frame does not arrive in time.
const unsigned int NXDN_FRAME_TIME = 80U;

// The length of a NXDN voice data packet (NXDND) as passed over the network.
const unsigned int NXDN_VOICE_LENGTH = 43U;

// The length of a NXDN poll packet (NXDNP).
const unsigned int NXDN_POLL_LENGTH = 17U;

CNXDNNetwork::CNXDNNetwork(const std::string& address, unsigned int port, const std::string& callsign, bool debug, unsigned int jitter, bool repeat) :
m_socket(address, port),
m_callsign(callsign),
m_debug(debug),
m_address(),
m_port(0U),
m_jitter(jitter),
m_repeat(repeat),
m_delayBuffer(NULL),
m_buffer(NULL)
{
	m_callsign.resize(10U, ' ');

	// A Jitter value of 0 disables the receive jitter buffer entirely, so the
	// converter behaves exactly as it did before it existed.
	if (m_jitter > 0U) {
		m_buffer      = new unsigned char[BUFFER_LENGTH];
		// NXDN has no silence frame to fall back on, so the buffer always
		// repeats the last frame. Whether a repeat is actually handed out is
		// decided by the Repeat setting in read().
		m_delayBuffer = new CDelayBuffer("NXDN", NXDN_VOICE_LENGTH, NXDN_FRAME_TIME, m_jitter, m_debug, true);
	}
}

CNXDNNetwork::~CNXDNNetwork()
{
	delete m_delayBuffer;
	delete[] m_buffer;
}

bool CNXDNNetwork::open()
{
	LogMessage("Opening NXDN network connection");

	return m_socket.open();
}

void CNXDNNetwork::setDestination(const in_addr& address, unsigned int port)
{
	m_address = address;
	m_port    = port;
}

void CNXDNNetwork::clearDestination()
{
	m_address.s_addr = INADDR_NONE;
	m_port           = 0U;
}

bool CNXDNNetwork::write(const unsigned char* data, unsigned int length)
{
	assert(data != NULL);
	assert(length > 0U);

	if (m_debug)
		CUtils::dump(1U, "NXDN Network Data Sent", data, length);

	return m_socket.write(data, length, m_address, m_port);
}

bool CNXDNNetwork::write(const unsigned char* data, unsigned short srcId, unsigned short dstId, bool grp)
{
	assert(data != NULL);

	unsigned char buffer[50U];

	buffer[0U] = 'N';
	buffer[1U] = 'X';
	buffer[2U] = 'D';
	buffer[3U] = 'N';
	buffer[4U] = 'D';

	buffer[5U] = (srcId >> 8) & 0xFFU;
	buffer[6U] = (srcId >> 0) & 0xFFU;

	buffer[7U] = (dstId >> 8) & 0xFFU;
	buffer[8U] = (dstId >> 0) & 0xFFU;

	buffer[9U] = 0x00U;
	buffer[9U] |= grp ? 0x01U : 0x00U;

	if (data[0U] == 0x81U || data[0U] == 0x83U) {
		buffer[9U] |= data[5U] == 0x01U ? 0x04U : 0x00U;
		buffer[9U] |= data[5U] == 0x08U ? 0x08U : 0x00U;
	}

	::memcpy(buffer + 10U, data, 33U);

	if (m_debug)
		CUtils::dump(1U, "NXDN Network Data Sent", buffer, 43U);

	return m_socket.write(buffer, 43U, m_address, m_port);
}

unsigned int CNXDNNetwork::read(unsigned char* data)
{
	assert(data != NULL);

	in_addr address;
	unsigned int port;

	// Without a jitter buffer the socket data is handed back untouched, like it
	// was before the buffer existed.
	if (m_delayBuffer == NULL) {
		int len = m_socket.read(data, BUFFER_LENGTH, address, port);
		if (len <= 0)
			return 0U;

		// Invalid packet type?
		if (::memcmp(data, "NXDN", 4U) != 0)
			return 0U;

		if (len != 17 && len != 43)
			return 0U;

		if (m_debug)
			CUtils::dump(1U, "NXDN Network Data Received", data, len);

		return len;
	}

	// Drain everything the socket has. Voice frames are queued in the jitter
	// buffer, polls are returned straight away.
	for (;;) {
		int len = m_socket.read(m_buffer, BUFFER_LENGTH, address, port);
		if (len <= 0)
			break;

		// Invalid packet type?
		if (::memcmp(m_buffer, "NXDN", 4U) != 0)
			continue;

		if (m_debug)
			CUtils::dump(1U, "NXDN Network Data Received", m_buffer, len);

		if (len == (int)NXDN_VOICE_LENGTH) {
			m_delayBuffer->addData(m_buffer, NXDN_VOICE_LENGTH);
		} else if (len == (int)NXDN_POLL_LENGTH) {
			::memcpy(data, m_buffer, NXDN_POLL_LENGTH);
			return NXDN_POLL_LENGTH;
		}
	}

	// Hand out the next voice frame. A missing frame is only replaced by a
	// repeat of the last one when the Repeat setting asks for it, otherwise the
	// gap is left alone.
	unsigned int length = 0U;
	B_STATUS status = m_delayBuffer->getData(data, length);
	if (status == BS_DATA)
		return length;

	if (status == BS_MISSING && m_repeat)
		return length;

	return 0U;
}

void CNXDNNetwork::clock(unsigned int ms)
{
	if (m_delayBuffer != NULL)
		m_delayBuffer->clock(ms);
}

void CNXDNNetwork::reset()
{
	if (m_delayBuffer != NULL)
		m_delayBuffer->reset();
}

bool CNXDNNetwork::writePoll(unsigned short tg)
{
	unsigned char data[20U];

	data[0U] = 'N';
	data[1U] = 'X';
	data[2U] = 'D';
	data[3U] = 'N';
	data[4U] = 'P';

	for (unsigned int i = 0U; i < 10U; i++)
		data[i + 5U] = m_callsign.at(i);

	data[15U] = (tg >> 8) & 0xFFU;
	data[16U] = (tg >> 0) & 0xFFU;

	if (m_debug)
		CUtils::dump(1U, "NXDN Network Poll Sent", data, 17U);

	return m_socket.write(data, 17U, m_address, m_port);
}

bool CNXDNNetwork::writeUnlink(unsigned short tg)
{
	unsigned char data[20U];

	data[0U] = 'N';
	data[1U] = 'X';
	data[2U] = 'D';
	data[3U] = 'N';
	data[4U] = 'U';

	for (unsigned int i = 0U; i < 10U; i++)
		data[i + 5U] = m_callsign.at(i);

	data[15U] = (tg >> 8) & 0xFFU;
	data[16U] = (tg >> 0) & 0xFFU;

	if (m_debug)
		CUtils::dump(1U, "NXDN Network Unlink Sent", data, 17U);

	return m_socket.write(data, 17U, m_address, m_port);
}

void CNXDNNetwork::close()
{
	m_socket.close();

	LogMessage("Closing NXDN network connection");
}
