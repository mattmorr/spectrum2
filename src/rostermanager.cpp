/**
 * XMPP - libpurple transport
 *
 * Copyright (C) 2009, Jan Kaluza <hanzz@soc.pidgin.im>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include "transport/rostermanager.h"
#include "transport/rosterstorage.h"
#include "transport/storagebackend.h"
#include "transport/buddy.h"
#include "transport/usermanager.h"
#include "transport/buddy.h"
#include "transport/user.h"
#include "Swiften/Roster/SetRosterRequest.h"
#include "Swiften/Elements/RosterPayload.h"
#include "Swiften/Elements/RosterItemPayload.h"
#include "Swiften/Elements/RosterItemExchangePayload.h"
#include "log4cxx/logger.h"

using namespace log4cxx;

namespace Transport {

static LoggerPtr logger = Logger::getLogger("RosterManager");

RosterManager::RosterManager(User *user, Component *component){
	m_rosterStorage = NULL;
	m_user = user;
	m_component = component;
	m_setBuddyTimer = m_component->getNetworkFactories()->getTimerFactory()->createTimer(1000);
	m_RIETimer = m_component->getNetworkFactories()->getTimerFactory()->createTimer(5000);
	m_RIETimer->onTick.connect(boost::bind(&RosterManager::sendRIE, this));
}

RosterManager::~RosterManager() {
	m_setBuddyTimer->stop();
	m_RIETimer->stop();
	if (m_rosterStorage) {
// 		for (std::map<std::string, Buddy *>::const_iterator it = m_buddies.begin(); it != m_buddies.end(); it++) {
// 			Buddy *buddy = (*it).second;
// 			m_rosterStorage->storeBuddy(buddy);
// 		}
		m_rosterStorage->storeBuddies();
	}

	sendUnavailablePresences(m_user->getJID().toBare());

	for (std::map<std::string, Buddy *>::const_iterator it = m_buddies.begin(); it != m_buddies.end(); it++) {
		Buddy *buddy = (*it).second;
		if (!buddy) {
			continue;
		}
		delete buddy;
	}

	if (m_rosterStorage)
		delete m_rosterStorage;
}

void RosterManager::setBuddy(Buddy *buddy) {
// 	m_setBuddyTimer->onTick.connect(boost::bind(&RosterManager::setBuddyCallback, this, buddy));
// 	m_setBuddyTimer->start();
	setBuddyCallback(buddy);
}

void RosterManager::sendBuddyRosterPush(Buddy *buddy) {
	Swift::RosterPayload::ref payload = Swift::RosterPayload::ref(new Swift::RosterPayload());
	Swift::RosterItemPayload item;
	item.setJID(buddy->getJID().toBare());
	item.setName(buddy->getAlias());
	item.setGroups(buddy->getGroups());
	item.setSubscription(Swift::RosterItemPayload::Both);

	payload->addItem(item);

	Swift::SetRosterRequest::ref request = Swift::SetRosterRequest::create(payload, m_user->getJID().toBare(), m_component->getIQRouter());
	request->onResponse.connect(boost::bind(&RosterManager::handleBuddyRosterPushResponse, this, _1, buddy->getName()));
	request->send();
}

void RosterManager::setBuddyCallback(Buddy *buddy) {
	m_setBuddyTimer->onTick.disconnect(boost::bind(&RosterManager::setBuddyCallback, this, buddy));

	if (m_rosterStorage) {
		buddy->onBuddyChanged.connect(boost::bind(&RosterStorage::storeBuddy, m_rosterStorage, buddy));
	}

	LOG4CXX_INFO(logger, "Associating buddy " << buddy->getName() << " with " << m_user->getJID().toString());
	m_buddies[buddy->getName()] = buddy;
	onBuddySet(buddy);

	// In server mode the only way is to send jabber:iq:roster push.
	// In component mode we send RIE or Subscribe presences, based on features.
	if (m_component->inServerMode()) {
		sendBuddyRosterPush(buddy);
	}
	else {
		if (m_setBuddyTimer->onTick.empty()) {
			m_setBuddyTimer->stop();
			if (true /*&& rie_is_supported*/) {
				m_RIETimer->start();
			}
		}
	}

	if (m_rosterStorage)
		m_rosterStorage->storeBuddy(buddy);
}

void RosterManager::unsetBuddy(Buddy *buddy) {
	m_buddies.erase(buddy->getName());
	if (m_rosterStorage)
		m_rosterStorage->removeBuddyFromQueue(buddy);
	onBuddyUnset(buddy);
}

void RosterManager::storeBuddy(Buddy *buddy) {
	if (m_rosterStorage) {
		m_rosterStorage->storeBuddy(buddy);
	}
}

void RosterManager::handleBuddyRosterPushResponse(Swift::ErrorPayload::ref error, const std::string &key) {
	if (m_buddies[key] != NULL) {
		m_buddies[key]->handleBuddyChanged();
	}
}

Buddy *RosterManager::getBuddy(const std::string &name) {
	return m_buddies[name];
}

void RosterManager::sendRIE() {
	m_RIETimer->stop();

	Swift::Presence::ref highest = m_component->getPresenceOracle()->getHighestPriorityPresence(m_user->getJID().toBare());

	LOG4CXX_INFO(logger, "Sending RIE stanza to " << highest->getFrom().toString());

	Swift::RosterItemExchangePayload::ref payload = Swift::RosterItemExchangePayload::ref(new Swift::RosterItemExchangePayload());
	for (std::map<std::string, Buddy *>::const_iterator it = m_buddies.begin(); it != m_buddies.end(); it++) {
		Buddy *buddy = (*it).second;
		if (!buddy) {
			continue;
		}
		Swift::RosterItemExchangePayload::Item item;
		item.setJID(buddy->getJID().toBare());
		item.setName(buddy->getAlias());
		item.setAction(Swift::RosterItemExchangePayload::Item::Add);
// 		item.setGroups(buddy->getGroups());

		payload->addItem(item);
	}

	boost::shared_ptr<Swift::GenericRequest<Swift::RosterItemExchangePayload> > request(new Swift::GenericRequest<Swift::RosterItemExchangePayload>(Swift::IQ::Set, highest->getFrom(), payload, m_component->getIQRouter()));
	request->send();
}

void RosterManager::handleSubscription(Swift::Presence::ref presence) {
	std::string legacyName = Buddy::JIDToLegacyName(presence->getTo());
	// For server mode the subscription changes are handler in rosterresponder.cpp
	// using roster pushes.
	if (m_component->inServerMode()) {
		Swift::Presence::ref response = Swift::Presence::create();
		response->setTo(presence->getFrom());
		response->setFrom(presence->getTo());
		Buddy *buddy = getBuddy(Buddy::JIDToLegacyName(presence->getTo()));
		if (buddy) {
			LOG4CXX_INFO(logger, m_user->getJID().toString() << ": Subscription received and buddy " << Buddy::JIDToLegacyName(presence->getTo()) << " is already there => answering");
			switch (presence->getType()) {
				case Swift::Presence::Subscribe:
					response->setType(Swift::Presence::Subscribed);
					break;
				case Swift::Presence::Unsubscribe:
					response->setType(Swift::Presence::Unsubscribed);
					break;
				default:
					return;
			}
			m_component->getStanzaChannel()->sendPresence(response);
			
		}
		else {
			BuddyInfo buddyInfo;
			switch (presence->getType()) {
				// buddy is not in roster, so add him
				case Swift::Presence::Subscribe:
					buddyInfo.id = -1;
					buddyInfo.alias = "";
					buddyInfo.legacyName = Buddy::JIDToLegacyName(presence->getTo());
					buddyInfo.subscription = "both";
					buddyInfo.flags = 0;
					LOG4CXX_INFO(logger, m_user->getJID().toString() << ": Subscription received for new buddy " << buddyInfo.legacyName << " => adding to legacy network");

					buddy = m_component->getFactory()->createBuddy(this, buddyInfo);
					setBuddy(buddy);
					onBuddyAdded(buddy);
					response->setType(Swift::Presence::Subscribed);
					break;
				// buddy is already there, so nothing to do, just answer
				case Swift::Presence::Unsubscribe:
					response->setType(Swift::Presence::Unsubscribed);
					break;
				default:
					return;
			}
			m_component->getStanzaChannel()->sendPresence(response);
		}
	}
	else {
		Swift::Presence::ref response = Swift::Presence::create();
		Swift::Presence::ref currentPresence;
		response->setTo(presence->getFrom());
		response->setFrom(presence->getTo());

		Buddy *buddy = getBuddy(Buddy::JIDToLegacyName(presence->getTo()));
		if (buddy) {
			switch (presence->getType()) {
				// buddy is already there, so nothing to do, just answer
				case Swift::Presence::Subscribe:
					response->setType(Swift::Presence::Subscribed);
					currentPresence = buddy->generatePresenceStanza(255);
					if (currentPresence) {
						currentPresence->setTo(presence->getFrom());
						m_component->getStanzaChannel()->sendPresence(currentPresence);
					}
					break;
				// remove buddy
				case Swift::Presence::Unsubscribe:
					response->setType(Swift::Presence::Unsubscribed);
					onBuddyRemoved(buddy);
					break;
				// just send response
				case Swift::Presence::Unsubscribed:
// 					response->setType(Swift::Presence::Unsubscribe);
					break;
				default:
					return;
			}
		}
		else {
			BuddyInfo buddyInfo;
			switch (presence->getType()) {
				// buddy is not in roster, so add him
				case Swift::Presence::Subscribe:
					buddyInfo.id = -1;
					buddyInfo.alias = "";
					buddyInfo.legacyName = Buddy::JIDToLegacyName(presence->getTo());
					buddyInfo.subscription = "both";
					buddyInfo.flags = 0;

					buddy = m_component->getFactory()->createBuddy(this, buddyInfo);
					setBuddy(buddy);
					onBuddyAdded(buddy);
					response->setType(Swift::Presence::Subscribed);
					break;
				// buddy is already there, so nothing to do, just answer
				case Swift::Presence::Unsubscribe:
					response->setType(Swift::Presence::Unsubscribed);
					break;
				// just send response
				case Swift::Presence::Unsubscribed:
					response->setType(Swift::Presence::Unsubscribe);
					break;
				// just send response
				default:
					return;
			}
		}

		m_component->getStanzaChannel()->sendPresence(response);

		// We have to act as buddy and send its subscribe/unsubscribe just to be sure...
		switch (response->getType()) {
			case Swift::Presence::Unsubscribed:
				response->setType(Swift::Presence::Unsubscribe);
				m_component->getStanzaChannel()->sendPresence(response);
				break;
			case Swift::Presence::Subscribed:
				response->setType(Swift::Presence::Subscribe);
				m_component->getStanzaChannel()->sendPresence(response);
				break;
			default:
				break;
		}
	}
}

void RosterManager::setStorageBackend(StorageBackend *storageBackend) {
	if (m_rosterStorage || !storageBackend) {
		return;
	}
	m_rosterStorage = new RosterStorage(m_user, storageBackend);

	std::list<BuddyInfo> roster;
	storageBackend->getBuddies(m_user->getUserInfo().id, roster);

	for (std::list<BuddyInfo>::const_iterator it = roster.begin(); it != roster.end(); it++) {
		Buddy *buddy = m_component->getFactory()->createBuddy(this, *it);
		LOG4CXX_INFO(logger, m_user->getJID().toString() << ": Adding cached buddy " << buddy->getName() << " fom database");
		m_buddies[buddy->getName()] = buddy;
		buddy->onBuddyChanged.connect(boost::bind(&RosterStorage::storeBuddy, m_rosterStorage, buddy));
		onBuddySet(buddy);
	}
}

Swift::RosterPayload::ref RosterManager::generateRosterPayload() {
	Swift::RosterPayload::ref payload = Swift::RosterPayload::ref(new Swift::RosterPayload());

	for (std::map<std::string, Buddy *>::const_iterator it = m_buddies.begin(); it != m_buddies.end(); it++) {
		Buddy *buddy = (*it).second;
		if (!buddy) {
			continue;
		}
		Swift::RosterItemPayload item;
		item.setJID(buddy->getJID().toBare());
		item.setName(buddy->getAlias());
		item.setGroups(buddy->getGroups());
		item.setSubscription(Swift::RosterItemPayload::Both);
		payload->addItem(item);
	}
	return payload;
}

void RosterManager::sendCurrentPresences(const Swift::JID &to) {
	for (std::map<std::string, Buddy *>::const_iterator it = m_buddies.begin(); it != m_buddies.end(); it++) {
		Buddy *buddy = (*it).second;
		if (!buddy) {
			continue;
		}
		Swift::Presence::ref presence = buddy->generatePresenceStanza(255);
		if (presence) {
			presence->setTo(to);
			m_component->getStanzaChannel()->sendPresence(presence);
		}
	}
}

void RosterManager::sendUnavailablePresences(const Swift::JID &to) {
	for (std::map<std::string, Buddy *>::const_iterator it = m_buddies.begin(); it != m_buddies.end(); it++) {
		Buddy *buddy = (*it).second;
		if (!buddy) {
			continue;
		}
		Swift::Presence::ref presence = buddy->generatePresenceStanza(255);
		if (presence) {
			presence->setTo(to);
			presence->setType(Swift::Presence::Unavailable);
			m_component->getStanzaChannel()->sendPresence(presence);
		}
	}
}

}
