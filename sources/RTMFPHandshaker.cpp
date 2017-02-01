/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "RTMFPHandshaker.h"
#include "RTMFPSession.h"
#include "RTMFPSender.h"

using namespace Mona;
using namespace std;

RTMFPHandshaker::RTMFPHandshaker(RTMFPSession* pSession) : _pSession(pSession), _name("handshaker") {
}

RTMFPHandshaker::~RTMFPHandshaker() {
}

void RTMFPHandshaker::close() {

	_mapTags.clear();
	_mapCookies.clear();
}

void RTMFPHandshaker::process(const SocketAddress& address, PoolBuffer& pBuffer) {
	if (!BandWriter::decode(address, pBuffer))
		return;

	BinaryReader reader(pBuffer.data(), pBuffer->size());
	reader.next(2); // TODO: CRC, don't share this part in onPacket() 
	
	_address.set(address); // update address

	if (Logs::GetLevel() >= 7)
		DUMP("RTMFP", reader.current(), reader.available(), "Request from ", address.toString())

	UInt8 marker = reader.read8();
	_timeReceived = reader.read16();
	_lastReceptionTime.update();

	// Handshake
	if (marker != 0x0B) {
		WARN("Unexpected Handshake marker : ", Format<UInt8>("%02x", marker));
		return;
	}

	UInt8 type = reader.read8();
	UInt16 length = reader.read16();
	reader.shrink(length); // resize the buffer to ignore the padding bytes

	switch (type) {
	case 0x30:
		handleHandshake30(reader); break; // P2P only (and send handshake 70)
	case 0x38:
		sendHandshake78(reader); break; // P2P only
	case 0x70:
		handleHandshake70(reader); break; // (and send handshake 38)
	case 0x71:
		handleRedirection(reader); break; // p2p address exchange or server redirection
	default:
		ERROR("Unexpected p2p handshake type : ", Format<UInt8>("%.2x", (UInt8)type))
		break;
	}
}

bool  RTMFPHandshaker::startHandshake(shared_ptr<Handshake>& pHandshake, const SocketAddress& address, FlowManager* pSession, bool responder, bool p2p) {
	PEER_LIST_ADDRESS_TYPE mapAddresses;
	return startHandshake(pHandshake, address, mapAddresses, pSession, responder, p2p);
}

bool  RTMFPHandshaker::startHandshake(shared_ptr<Handshake>& pHandshake, const SocketAddress& address, const PEER_LIST_ADDRESS_TYPE& addresses, FlowManager* pSession, bool responder, bool p2p) {
	const string& tag = pSession->tag();
	auto itHandshake = _mapTags.lower_bound(tag);
	if (itHandshake == _mapTags.end() || itHandshake->first != tag) {
		itHandshake = _mapTags.emplace_hint(itHandshake, piecewise_construct, forward_as_tuple(tag), forward_as_tuple(new Handshake(pSession, address, addresses, p2p)));
		itHandshake->second->pTag = &itHandshake->first;
		pHandshake = itHandshake->second;
		return true;
	}
	WARN("Handshake already exists, nothing done")
	pHandshake = itHandshake->second;
	return false;
}

void RTMFPHandshaker::sendHandshake70(const string& tag, const SocketAddress& address, const SocketAddress& host) {
	auto itHandshake = _mapTags.lower_bound(tag);
	if (itHandshake == _mapTags.end() || itHandshake->first != tag) {
		PEER_LIST_ADDRESS_TYPE addresses;
		addresses.emplace(address, RTMFP::ADDRESS_PUBLIC);
		itHandshake = _mapTags.emplace_hint(itHandshake, piecewise_construct, forward_as_tuple(tag.c_str()), forward_as_tuple(new Handshake(NULL, host, addresses, true)));
		itHandshake->second->pTag = &itHandshake->first;
		TRACE("Creating handshake for tag ", Util::FormatHex(BIN itHandshake->second->pTag->c_str(), itHandshake->second->pTag->size(), LOG_BUFFER))
	}
	else { // Add the address if unknown
		auto itAddress = itHandshake->second->listAddresses.lower_bound(address);
		if (itAddress == itHandshake->second->listAddresses.end() || itAddress->first != address)
			itHandshake->second->listAddresses.emplace_hint(itAddress, address, RTMFP::ADDRESS_PUBLIC);
	}
	// TODO: see if we must remove handshake from _mapTags
	_address.set(address); // set address before sending
	sendHandshake70(tag, itHandshake->second);
}

void RTMFPHandshaker::manage() {

	// Ask server to send p2p addresses
	auto itHandshake = _mapTags.begin();
	while (itHandshake != _mapTags.end()) {
		shared_ptr<Handshake> pHandshake = itHandshake->second;
		if (pHandshake->pSession && !pHandshake->pCookie && (!pHandshake->attempt || pHandshake->lastAttempt.isElapsed(pHandshake->attempt * 1500))) {
			if (pHandshake->attempt++ == 11) {
				DEBUG("Connection to ", pHandshake->pSession->name(), " has reached 11 attempt without answer, closing...")
				_mapTags.erase(itHandshake++);
				continue;
			}

			DEBUG("Sending new handshake 30 to server (target : ", pHandshake->pSession->name(), "; ", pHandshake->attempt, "/11)")
			if (pHandshake->hostAddress) {
				_address.set(pHandshake->hostAddress);
				sendHandshake30(pHandshake->pSession->epd(), itHandshake->first);
			}
			// If we are not in p2p mode we must send to all known addresses
			if (!pHandshake->isP2P) {
				for (auto itAddresses : pHandshake->listAddresses) {
					_address.set(itAddresses.first);
					sendHandshake30(pHandshake->pSession->epd(), itHandshake->first);
				}
			}
			pHandshake->lastAttempt.update();
		}
		++itHandshake;
	}
}

void RTMFPHandshaker::sendHandshake30(const string& epd, const string& tag) {
	// (First packets are encoded with default key)
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write7BitLongValue(epd.size());
	writer.write(epd.data(), epd.size());

	writer.write(tag);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x30).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size());
}

void RTMFPHandshaker::handleHandshake30(BinaryReader& reader) {

	UInt64 peerIdSize = reader.read7BitLongValue();
	if (peerIdSize != 0x22)
		ERROR("Unexpected peer id size : ", peerIdSize, " (expected 34)")
	else if ((peerIdSize = reader.read7BitLongValue()) != 0x21)
		ERROR("Unexpected peer id size : ", peerIdSize, " (expected 33)")
	else if (reader.read8() != 0x0F)
		ERROR("Unexpected marker : ", *reader.current(), " (expected 0x0F)")
	else {

		string buff, peerId, tag;
		reader.read(0x20, buff);
		reader.read(16, tag);
		Util::FormatHex(BIN buff.data(), buff.size(), peerId);
		if (peerId != _pSession->peerId()) {
			WARN("Incorrect Peer ID in p2p handshake 30 : ", peerId)
			return;
		}

		sendHandshake70(tag, _address, _pSession->address());
	}
}

void RTMFPHandshaker::sendHandshake70(const string& tag, shared_ptr<Handshake>& pHandshake) {

	if (!pHandshake->pCookie) {
		string cookie(COOKIE_SIZE, '\0');
		Util::Random(BIN cookie.data(), COOKIE_SIZE);
		TRACE("Creating cookie ", Util::FormatHex(BIN cookie.data(), cookie.size(), LOG_BUFFER))
		auto itCookie = _mapCookies.emplace(piecewise_construct, forward_as_tuple(cookie), forward_as_tuple(pHandshake)).first;
		pHandshake->pCookie = &itCookie->first;
	}	

	// Write Response
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write8(16);
	writer.write(tag);

	writer.write8(COOKIE_SIZE);
	writer.write(BIN pHandshake->pCookie->c_str(), COOKIE_SIZE);

	Exception ex;
	DiffieHellman* pDh(NULL);
	if (!diffieHellman(pDh))
		return;
	pHandshake->pubKey.resize(pDh->publicKeySize(ex));
	pDh->readPublicKey(ex, pHandshake->pubKey.data());
	writer.write7BitValue(pHandshake->pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(pHandshake->pubKey);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x70).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size());
}

void RTMFPHandshaker::handleHandshake70(BinaryReader& reader) {
	string tagReceived, cookie, farKey;

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		WARN("Unexpected tag size : ", tagSize)
		return;
	}
	reader.read(16, tagReceived);
	auto itHandshake = _mapTags.find(tagReceived);
	if (itHandshake == _mapTags.end()) {
		DEBUG("Unexpected tag received from ", _address.toString(), ", possible old request")
		return;
	}
	shared_ptr<Handshake> pHandshake = itHandshake->second;
	if (!pHandshake->pSession) {
		WARN("Unexpected handshake 70 received on responder session")
		return;
	}

	// Normal NetConnection
	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ERROR("Unexpected cookie size : ", cookieSize)
		return;
	}
	reader.read(cookieSize, cookie);

	if (!pHandshake->isP2P) {
		string certificat;
		reader.read(77, certificat);
		DEBUG("Server Certificate : ", Util::FormatHex(BIN certificat.data(), 77, LOG_BUFFER))
	}
	else {
		UInt32 keySize = (UInt32)reader.read7BitLongValue() - 2;
		if (keySize != 0x80 && keySize != 0x7F) {
			ERROR("Unexpected responder key size : ", keySize)
			return;
		}
		if (reader.read16() != 0x1D02) {
			ERROR("Unexpected signature before responder key (expected 1D02)")
			return;
		}
		reader.read(keySize, pHandshake->farKey);
	}

	// Handshake 70 accepted? => We send the handshake 38
	if (pHandshake->pSession->onPeerHandshake70(_address, pHandshake->farKey, cookie))
		sendHandshake38(pHandshake, cookie);
}

void RTMFPHandshaker::sendHandshake38(const shared_ptr<Handshake>& pHandshake, const string& cookie) {

	// Write handshake
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(pHandshake->pSession->sessionId()); // id

	writer.write7BitLongValue(cookie.size());
	writer.write(cookie); // Resend cookie

	// TODO: refactorize
	Exception ex;
	DiffieHellman* pDh(NULL);
	if (!diffieHellman(pDh))
		return;
	pHandshake->pubKey.resize(pDh->publicKeySize(ex));
	pDh->readPublicKey(ex, pHandshake->pubKey.data());
	writer.write7BitLongValue(pHandshake->pubKey.size() + 4);

	UInt32 idPos = writer.size();
	writer.write7BitValue(pHandshake->pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(pHandshake->pubKey);

	// Build and save Peer ID if it is RTMFPSession
	pHandshake->pSession->buildPeerID(writer.data() + idPos, writer.size() - idPos);

	BinaryWriter nonceWriter(pHandshake->nonce.data(), 0x4C);
	nonceWriter.write(EXPAND("\x02\x1D\x02\x41\x0E"));
	Util::Random(pHandshake->nonce.data() + 5, 64); // nonce 64 random bytes
	nonceWriter.next(64);
	nonceWriter.write(EXPAND("\x03\x1A\x02\x0A\x02\x1E\x02"));
	writer.write7BitValue(pHandshake->nonce.size());
	writer.write(pHandshake->nonce);
	writer.write8(0x58);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x38).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size());
	pHandshake->pSession->status = RTMFP::HANDSHAKE38;
}


void RTMFPHandshaker::sendHandshake78(BinaryReader& reader) {

	UInt32 farId = reader.read32(); // id session

	string cookie;
	if (reader.read8() != 0x40) {
		ERROR("Cookie size should be 64 bytes but found : ", *(reader.current() - 1))
		return;
	}
	reader.read(0x40, cookie);
	auto itHandshake = _mapCookies.find(cookie);
	if (itHandshake == _mapCookies.end()) {
		DEBUG("No cookie found for handshake 38, possible old request, ignored")
		return;
	}
	shared_ptr<Handshake> pHandshake = itHandshake->second;

	UInt32 publicKeySize = reader.read7BitValue();
	if (publicKeySize != 0x84)
		DEBUG("Public key size should be 132 bytes but found : ", publicKeySize)
	UInt32 idPos = reader.position(); // record position for peer ID determination
	if ((publicKeySize = reader.read7BitValue()) != 0x82)
		DEBUG("Public key size should be 130 bytes but found : ", publicKeySize)
	UInt16 signature = reader.read16();
	if (signature != 0x1D02) {
		ERROR("Expected signature 1D02 but found : ", Format<UInt16>("%.4x", signature))
		removeHandshake(pHandshake);
		return;
	}
	reader.read(publicKeySize - 2, pHandshake->farKey);

	UInt32 nonceSize = reader.read7BitValue();
	if (nonceSize != 0x4C) {
		ERROR("Responder Nonce size should be 76 bytes but found : ", nonceSize)
		removeHandshake(pHandshake);
		return;
	}
	reader.read(nonceSize, pHandshake->farNonce);
	pHandshake->farNonce.resize(nonceSize);

	UInt8 endByte;
	if ((endByte = reader.read8()) != 0x58) {
		ERROR("Unexpected end byte : ", endByte, " (expected 0x58)")
		removeHandshake(pHandshake);
		return;
	}

	// Build peer ID and update the parent
	string rawId("\x21\x0f"), peerId;
	UInt8 id[PEER_ID_SIZE];
	EVP_Digest(reader.data() + idPos, publicKeySize + 2, id, NULL, EVP_sha256(), NULL);
	rawId.append(STR id, PEER_ID_SIZE);
	Util::FormatHex(id, PEER_ID_SIZE, peerId);
	DEBUG("peer ID calculated from public key : ", peerId)

	// Create the session, if already exists and connected we ignore the request
	if (!_pSession->onNewPeerId(_address, pHandshake, farId, rawId, peerId)) {
		removeHandshake(pHandshake);
		return;
	}
	FlowManager* pSession = pHandshake->pSession;

	// Write Response
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(pSession->sessionId());
	writer.write8(0x49); // nonce is 73 bytes long
	pHandshake->nonce.resize(0x49, false);
	BinaryWriter nonceWriter(pHandshake->nonce.data(), 0x49);
	nonceWriter.write(EXPAND("\x03\x1A\x00\x00\x02\x1E\x00\x41\x0E"));
	Util::Random(pHandshake->nonce.data() + 9, 0x40); // nonce 64 random bytes
	writer.write(pHandshake->nonce.data(), pHandshake->nonce.size());
	writer.write8(0x58);

	// Important: send this before computing encoder key because we need the default encoder
	_farId = farId;
	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x78).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size());
	_farId = 0; // reset far Id to default

	// Compute P2P keys for decryption/encryption
	pSession->computeKeys(farId);
}

void RTMFPHandshaker::handleRedirection(BinaryReader& reader) {

	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ERROR("Unexpected tag size : ", tagSize)
			return;
	}
	string tagReceived;
	reader.read(16, tagReceived);

	auto itTag = _mapTags.find(tagReceived);
	if (itTag == _mapTags.end()) {
		DEBUG("Unexpected tag received from ", _address.toString(), ", possible old request")
		return;
	}
	shared_ptr<Handshake> pHandshake(itTag->second);

	if (!pHandshake->pSession) {
		WARN("Unable to find the session related to handshake 71 from ", _address.toString())
		return;
	} else if (pHandshake->pSession->status > RTMFP::HANDSHAKE30) {
		DEBUG("Redirection message ignored, we have already received handshake 70")
		return;
	}

	// Read addresses
	RTMFP::ReadAddresses(reader, pHandshake->listAddresses, pHandshake->hostAddress);

	if (pHandshake->isP2P) {
		DEBUG("Server has sent to us the peer addresses of responders") // (we are the initiator)
		for (auto itAddresses : pHandshake->listAddresses) {
			_address.set(itAddresses.first);
			sendHandshake30(pHandshake->pSession->epd(), tagReceived);
		}
	} else
		DEBUG("Server redirection messsage, sending back the handshake 30")
}

void RTMFPHandshaker::flush(UInt8 marker, UInt32 size) {
	if (!_pSender)
		return;
	_pSender->packet.clear(size);
	BandWriter::flush(false, marker);
}

bool	RTMFPHandshaker::diffieHellman(Mona::DiffieHellman * &pDh) {
	if (!_diffieHellman.initialized()) {
		Mona::Exception ex;
		if (!_diffieHellman.initialize(ex)) {
			ERROR("Unable to initialize diffie hellman object : ", ex.error())
			return false;
		}
	}
	pDh = &_diffieHellman;
	return true;
}

UDPSocket& RTMFPHandshaker::socket(Mona::IPAddress::Family family) { 
	return _pSession->socket(family); 
}

// Return true if the session has failed
bool RTMFPHandshaker::failed() { 
	return _pSession->failed(); 
}

// Return the pool buffers object
const PoolBuffers&	RTMFPHandshaker::poolBuffers() { 
	return _pSession->poolBuffers();
}

// Remove the handshake properly
void RTMFPHandshaker::removeHandshake(std::shared_ptr<Handshake>& pHandshake) {

	// We can now erase the handshake object
	if (pHandshake->pCookie)
		_mapCookies.erase(*pHandshake->pCookie);
	if (pHandshake->pTag)
		_mapTags.erase(*pHandshake->pTag);
	pHandshake->pCookie = pHandshake->pTag = NULL;
	pHandshake.reset();
}
