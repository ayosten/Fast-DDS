/*************************************************************************
 * Copyright (c) 2014 eProsima. All rights reserved.
 *
 * This copy of eProsima Fast RTPS is licensed to you under the terms described in the
 * FASTRTPS_LIBRARY_LICENSE file included in this distribution.
 *
 *************************************************************************/

/**
 * @file RTPSParticipant.cpp
 *
 */

#include "RTPSParticipantImpl.h"

#include <fastrtps/rtps/resources/ResourceSend.h>
#include <fastrtps/rtps/resources/ResourceEvent.h>
#include <fastrtps/rtps/resources/ListenResource.h>

#include <fastrtps/rtps/messages/MessageReceiver.h>

#include <fastrtps/rtps/writer/StatelessWriter.h>
#include <fastrtps/rtps/writer/StatefulWriter.h>

#include <fastrtps/rtps/reader/StatelessReader.h>
#include <fastrtps/rtps/reader/StatefulReader.h>

#include <fastrtps/rtps/participant/RTPSParticipant.h>

#include <fastrtps/rtps/RTPSDomain.h>

#include <fastrtps/rtps/builtin/BuiltinProtocols.h>
#include <fastrtps/rtps/builtin/discovery/participant/PDPSimple.h>

#include <fastrtps/utils/IPFinder.h>
#include <fastrtps/utils/eClock.h>

#include <boost/thread.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/thread/lock_guard.hpp>

#include <fastrtps/utils/RTPSLog.h>



namespace eprosima {
namespace fastrtps{
namespace rtps {


static const char* const CLASS_NAME = "RTPSParticipantImpl";

static EntityId_t TrustedWriter(const EntityId_t& reader)
{
	if(reader == c_EntityId_SPDPReader) return c_EntityId_SPDPWriter;
	if(reader == c_EntityId_SEDPPubReader) return c_EntityId_SEDPPubWriter;
	if(reader == c_EntityId_SEDPSubReader) return c_EntityId_SEDPSubWriter;
	if(reader == c_EntityId_ReaderLiveliness) return c_EntityId_WriterLiveliness;

	return c_EntityId_Unknown;
}

Locator_t RTPSParticipant::applyLocatorAdaptRule(Locator_t loc){
	switch ((*loc).kind){
	case LOCATOR_KIND_UDPv4:
		//This is a completely made up rule
		(*loc).port += 10;
		break;
	case LOCATOR_KIND_UDPv6:
		//TODO - Define the rules
		break;
	}

}

RTPSParticipantImpl::RTPSParticipantImpl(const RTPSParticipantAttributes& PParam,
		const GuidPrefix_t& guidP,
		RTPSParticipant* par,
		RTPSParticipantListener* plisten):	m_guid(guidP,c_EntityId_RTPSParticipant),
				mp_event_thr(nullptr),
				mp_builtinProtocols(nullptr),
				mp_ResourceSemaphore(new boost::interprocess::interprocess_semaphore(0)),
				IdCounter(0),
				mp_participantListener(plisten),
				mp_userParticipant(par),
				mp_mutex(new boost::recursive_mutex()),
				m_threadID(0)

{
	const char* const METHOD_NAME = "RTPSParticipantImpl";
	boost::lock_guard<boost::recursive_mutex> guard(*mp_mutex);
	mp_userParticipant->mp_impl = this;
	m_att = PParam;
	Locator_t loc;
	loc.port = PParam.defaultSendPort;
	mp_event_thr = new ResourceEvent();
	mp_event_thr->init_thread(this);
	bool hasLocatorsDefined = true;
	//If no default locators are defined we define some.
	/* The reasoning here is the following.
		If the parameters of the RTPS Participant don't hold default listening locators for the creation
		of Endpoints, we make some for Unicast only.
		If there is at least one listen locator of any kind, we do not create any default ones.
		If there are no sending locators defined, we create default ones for the transports we implement.
	*/
	if(m_att.defaultUnicastLocatorList.empty() && m_att.defaultMulticastLocatorList.empty())
	{
		//Default Unicast Locators
		hasLocatorsDefined = false;
		Locator_t loc2;

		loc2.port=m_att.port.portBase+
				m_att.port.domainIDGain*PParam.builtin.domainId+
				m_att.port.offsetd3+
				m_att.port.participantIDGain*m_att.participantID;
		loc2.kind = LOCATOR_KIND_UDPv4;
		m_att.defaultUnicastLocatorList.push_back(loc2);
	}
	LocatorList_t defcopy = m_att.defaultUnicastLocatorList;
	m_att.defaultUnicastLocatorList.clear();

	/*	
		Since nothing guarantees the correct creation of the Resources on the Locators we have specified, and 
		in order to maintain synchrony between the defaultLocator list and the actuar ReceiveResources,
		We create the resources for these Locators now. Furthermore, in case these resources are taken, 
		we create them on another Locator and then update de defaultList.
	*/
	std::vector<ReceiverResource *> newItems;
	std::vector<ReceiverResource *> newItemsBuffer;

	for (auto it = defcopy.begin(); it != defcopy.end(); ++it){
		/* Try to build resources with that specific Locator*/
		newItemsBuffer = m_network_Factory.BuildReceiverResources((*it));
		while (newItems.empty()){
			//No ReceiverResources have been added, therefore we have to change the Locator 
			(*it) = applyLocatorAdaptRule(*it);											//Mutate the Locator to find a suitable rule. Overwrite the old one
																						//as it is useless now.
			newItemsBuffer = m_network_Factory.BuildReceiverResources((*it));	
		}
		//Now we DO have resources, and the new locator is already replacing the old one.
		newItems.insert(newItems.end(), newItemsBuffer.begin(), newItemsBuffer.end());
		newItemsBuffer.clear();
	}
	// 2 - For each generated element...
	for (auto it = newItems.begin(); it != newItems.end(); ++it){
		// 2.1 - Initialize a ReceiverResourceControlBlock
		ReceiverControlBlock newBlock{ std::move((*it)), std::vector<RTPSWriter *>(), std::vector<RTPSReader *>(), nullptr, boost::mutex(), nullptr };
		newBlock.mp_receiver = new MessageReceiver(listenSockSize);			//!! listenSockSize has to come from somewhere 
		// 2.2 - Push it to the list
		m_receiverResourcelist.push_back(newBlock);
	}
	// 4 - Launch the Listening thread for all of the uninitialized ReceiveResources
	for (auto it = m_receiverResourcelist.begin(); it != m_receiverResourcelist.end(); ++it){
		if ((*it)->mp_thread == nullptr)
			(*it)->mp_thread = new boost::thread(&RTPSParticipantImpl::performListenOperation, this, it);	//Bugfix
	}

	/*for(LocatorListIterator lit = defcopy.begin();lit!=defcopy.end();++lit)
	{
		ListenResource* LR = new ListenResource(this,++m_threadID,true);
		if(LR->init_thread(this,*lit,m_att.listenSocketBufferSize,false,false))
		{
			m_att.defaultUnicastLocatorList.push_back(LR->getListenLocators());
			this->m_listenResourceList.push_back(LR);
		}
		else
		{
			delete(LR);
		}
	}*/
	if(!hasLocatorsDefined)
		logInfo(RTPS_PARTICIPANT,m_att.getName()<<" Created with NO default Unicast Locator List, adding Locators: "<<m_att.defaultUnicastLocatorList);
	/*defcopy = m_att.defaultMulticastLocatorList;
	m_att.defaultMulticastLocatorList.clear();
	for(LocatorListIterator lit = defcopy.begin();lit!=defcopy.end();++lit)
	{
		ListenResource* LR = new ListenResource(this,++m_threadID,true);
		if(LR->init_thread(this,*lit,m_att.listenSocketBufferSize,true,false))
		{
			m_att.defaultMulticastLocatorList.push_back(LR->getListenLocators());
			this->m_listenResourceList.push_back(LR);
		}
		else
		{
			delete(LR);
		}
	}
	*/
	//Check if defaultOutLocatorsExist, create some if they don't
	hasLocatorsDefined = true;
	if (m_att.defaultOutLocatorList.empty()){
		hasLocatorsDefined = false;
		Locator_t SendLocator;
		/*TODO - Fill with desired default Send Locators for our transports*/


		m_att.defaultUnicastLocatorList.push_back(SendLocator);
	}
	if (!hasLocatorsDefined)
		logInfo(RTPS_PARTICIPANT, m_att.getName() << " Created with NO default Send Locator List, adding Locators: " << m_att.defaultOutLocatorList);

	logInfo(RTPS_PARTICIPANT,"RTPSParticipant \"" <<  m_att.getName() << "\" with guidPrefix: " <<m_guid.guidPrefix);
	//START BUILTIN PROTOCOLS
	mp_builtinProtocols = new BuiltinProtocols();
	if(!mp_builtinProtocols->initBuiltinProtocols(this,m_att.builtin))
	{
		logWarning(RTPS_PARTICIPANT, "The builtin protocols were not corecctly initialized");
	}
	//eClock::my_sleep(300);
}


RTPSParticipantImpl::~RTPSParticipantImpl()
{
	const char* const METHOD_NAME = "~RTPSParticipantImpl";
	logInfo(RTPS_PARTICIPANT,"removing "<<this->getGuid());


	while(m_userReaderList.size()>0)
		RTPSDomain::removeRTPSReader(*m_userReaderList.begin());

	while(m_userWriterList.size()>0)
		RTPSDomain::removeRTPSWriter(*m_userWriterList.begin());

	//Destruct ReceiverResources:
	for (auto it = m_receiverResourcelist.begin(); it != m_receiverResourcelist.end(); ++it)
		delete(*it);


	delete(this->mp_builtinProtocols);

	delete(this->mp_ResourceSemaphore);
	delete(this->mp_userParticipant);

	//Destruct SenderResources
	for (auto it = m_senderResource.begin; it != m_senderResource.end(); ++it)
		delete(*it);

	delete(this->mp_event_thr);
	delete(this->mp_mutex);
}

/*
 *
 * MAIN RTPSParticipant IMPL API
 *
 */


bool RTPSParticipantImpl::createWriter(RTPSWriter** WriterOut,
		WriterAttributes& param,WriterHistory* hist,WriterListener* listen, const EntityId_t& entityId,bool isBuiltin)
{
	const char* const METHOD_NAME = "createWriter";
	std::string type = (param.endpoint.reliabilityKind == RELIABLE) ? "RELIABLE" :"BEST_EFFORT";
	logInfo(RTPS_PARTICIPANT," of type " << type,C_B_YELLOW);
	EntityId_t entId;
	if(entityId== c_EntityId_Unknown)
	{
		if(param.endpoint.topicKind == NO_KEY)
			entId.value[3] = 0x03;
		else if(param.endpoint.topicKind == WITH_KEY)
			entId.value[3] = 0x02;
		uint32_t idnum;
		if(param.endpoint.getEntityID()>0)
			idnum = param.endpoint.getEntityID();
		else
		{
			IdCounter++;
			idnum = IdCounter;
		}

		octet* c = (octet*)&idnum;
		entId.value[2] = c[0];
		entId.value[1] = c[1];
		entId.value[0] = c[2];
		if(this->existsEntityId(entId,WRITER))
		{
			logError(RTPS_PARTICIPANT,"A writer with the same entityId already exists in this RTPSParticipant");
			return false;
		}
	}
	else
	{
		entId = entityId;
	}
	if(!param.endpoint.unicastLocatorList.isValid())
	{
		logError(RTPS_PARTICIPANT,"Unicast Locator List for Writer contains invalid Locator");
		return false;
	}
	if(!param.endpoint.multicastLocatorList.isValid())
	{
		logError(RTPS_PARTICIPANT,"Multicast Locator List for Writer contains invalid Locator");
		return false;
	}


	RTPSWriter* SWriter = nullptr;
	GUID_t guid(m_guid.guidPrefix,entId);
	if(param.endpoint.reliabilityKind == BEST_EFFORT)
		SWriter = (RTPSWriter*)new StatelessWriter(this,guid,param,hist,listen);
	else if(param.endpoint.reliabilityKind == RELIABLE)
		SWriter = (RTPSWriter*)new StatefulWriter(this,guid,param,hist,listen);

	if(SWriter==nullptr)
		return false;

	//SWriter->setListener(inlisten);
	//SWriter->setQos(param.qos,true);

	//Create SenderResources for this new Writer. 
	createSendResources((Endpoint *)SWriter);
	if(param.endpoint.reliabilityKind == RELIABLE)
	{
		if (!createAndAssociateReceiverswithEndpoint((Endpoint *)SWriter, isBuiltin))
		{
			delete(SWriter);
			return false;
		}
	}
	boost::lock_guard<boost::recursive_mutex> guard(*mp_mutex);
	m_allWriterList.push_back(SWriter);
	if(!isBuiltin)
		m_userWriterList.push_back(SWriter);
	*WriterOut = SWriter;
	return true;
}


bool RTPSParticipantImpl::createReader(RTPSReader** ReaderOut,
		ReaderAttributes& param,ReaderHistory* hist,ReaderListener* listen, const EntityId_t& entityId,bool isBuiltin, bool enable)
{
	const char* const METHOD_NAME = "createReader";
	std::string type = (param.endpoint.reliabilityKind == RELIABLE) ? "RELIABLE" :"BEST_EFFORT";
	logInfo(RTPS_PARTICIPANT," of type " << type,C_B_YELLOW);
	EntityId_t entId;
	if(entityId== c_EntityId_Unknown)
	{
		if(param.endpoint.topicKind == NO_KEY)
			entId.value[3] = 0x04;
		else if(param.endpoint.topicKind == WITH_KEY)
			entId.value[3] = 0x07;
		uint32_t idnum;
		if(param.endpoint.getEntityID()>0)
			idnum = param.endpoint.getEntityID();
		else
		{
			IdCounter++;
			idnum = IdCounter;
		}

		octet* c = (octet*)&idnum;
		entId.value[2] = c[0];
		entId.value[1] = c[1];
		entId.value[0] = c[2];
		if(this->existsEntityId(entId,WRITER))
		{
			logError(RTPS_PARTICIPANT,"A reader with the same entityId already exists in this RTPSParticipant");
			return false;
		}
	}
	else
	{
		entId = entityId;
	}
	if(!param.endpoint.unicastLocatorList.isValid())
	{
		logError(RTPS_PARTICIPANT,"Unicast Locator List for Reader contains invalid Locator");
		return false;
	}
	if(!param.endpoint.multicastLocatorList.isValid())
	{
		logError(RTPS_PARTICIPANT,"Multicast Locator List for Reader contains invalid Locator");
		return false;
	}
	RTPSReader* SReader = nullptr;
	GUID_t guid(m_guid.guidPrefix,entId);
	if(param.endpoint.reliabilityKind == BEST_EFFORT)
		SReader = (RTPSReader*)new StatelessReader(this,guid,param,hist,listen);
	else if(param.endpoint.reliabilityKind == RELIABLE)
		SReader = (RTPSReader*)new StatefulReader(this,guid,param,hist,listen);

	if(SReader==nullptr)
		return false;

	//SReader->setListener(inlisten);
	//SReader->setQos(param.qos,true);
	if (param.endpoint.reliabilityKind == RELIABLE)
		createSendResources((Endpoint *)SReader);

	if(isBuiltin)
	{
		SReader->setTrustedWriter(TrustedWriter(SReader->getGuid().entityId));
	}

    if(enable)
    {
		if (!createAndAssociateReceiverswithEndpoint((Endpoint *)SReader, isBuiltin))
        {
            delete(SReader);
            return false;
        }
    }

	boost::lock_guard<boost::recursive_mutex> guard(*mp_mutex);
	m_allReaderList.push_back(SReader);
	if(!isBuiltin)
		m_userReaderList.push_back(SReader);
	*ReaderOut = SReader;
	return true;
}

bool RTPSParticipantImpl::enableReader(RTPSReader *reader, bool isBuiltin)
{
    if(!assignEndpointListenResources((Endpoint*)reader,isBuiltin))
    {
        return false;
    }

    return true;
}




bool RTPSParticipantImpl::registerWriter(RTPSWriter* Writer,TopicAttributes& topicAtt,WriterQos& wqos)
{
	return this->mp_builtinProtocols->addLocalWriter(Writer,topicAtt,wqos);
}

bool RTPSParticipantImpl::registerReader(RTPSReader* reader,TopicAttributes& topicAtt,ReaderQos& rqos)
{
	return this->mp_builtinProtocols->addLocalReader(reader,topicAtt,rqos);
}

bool RTPSParticipantImpl::updateLocalWriter(RTPSWriter* Writer,WriterQos& wqos)
{
	return this->mp_builtinProtocols->updateLocalWriter(Writer,wqos);
}

bool RTPSParticipantImpl::updateLocalReader(RTPSReader* reader,ReaderQos& rqos)
{
	return this->mp_builtinProtocols->updateLocalReader(reader,rqos);
}

/*
 *
 * AUXILIARY METHODS
 *
 *  */


bool RTPSParticipantImpl::existsEntityId(const EntityId_t& ent,EndpointKind_t kind) const
{
	if(kind == WRITER)
	{
		for(std::vector<RTPSWriter*>::const_iterator it = m_userWriterList.begin();
				it!=m_userWriterList.end();++it)
		{
			if(ent == (*it)->getGuid().entityId)
				return true;
		}
	}
	else
	{
		for(std::vector<RTPSReader*>::const_iterator it = m_userReaderList.begin();
				it!=m_userReaderList.end();++it)
		{
			if(ent == (*it)->getGuid().entityId)
				return true;
		}
	}
	return false;
}


/*
 *
 * RECEIVER RESOURCE METHODS
 *
 */


bool RTPSParticipantImpl::assignEndpointListenResources(Endpoint* endp,bool isBuiltin)
{
	const char* const METHOD_NAME = "assignEndpointListenResources";
	bool valid = true;
	//boost::lock_guard<boost::recursive_mutex> guard(*endp->getMutex()); //  Fixed bug #914
	bool unicastempty = endp->getAttributes()->unicastLocatorList.empty();
	bool multicastempty = endp->getAttributes()->multicastLocatorList.empty();
    LocatorList_t uniList, mulList;

    if(!unicastempty)
        uniList = endp->getAttributes()->unicastLocatorList;
    if(!multicastempty)
        mulList = endp->getAttributes()->multicastLocatorList;

	if(unicastempty && !isBuiltin && multicastempty)
	{
		std::string auxstr = endp->getAttributes()->endpointKind == WRITER ? "WRITER" : "READER";
		logInfo(RTPS_PARTICIPANT,"Adding default Locator list to this " << auxstr);
		valid &= assignEndpoint2LocatorList(endp,m_att.defaultUnicastLocatorList,false,false);
        boost::lock_guard<boost::recursive_mutex> guard(*endp->getMutex());
		endp->getAttributes()->unicastLocatorList = m_att.defaultUnicastLocatorList;
	}
	else
	{
        valid &= assignEndpoint2LocatorList(endp, uniList, false, !isBuiltin);
        boost::lock_guard<boost::recursive_mutex> guard(*endp->getMutex());
        endp->getAttributes()->unicastLocatorList = uniList;
	}
	//MULTICAST
	if(multicastempty && !isBuiltin && unicastempty)
	{
		valid &= assignEndpoint2LocatorList(endp,m_att.defaultMulticastLocatorList,true,false);
        boost::lock_guard<boost::recursive_mutex> guard(*endp->getMutex());
		endp->getAttributes()->multicastLocatorList = m_att.defaultMulticastLocatorList;
	}
	else
	{
        valid &= assignEndpoint2LocatorList(endp, mulList, true, !isBuiltin);
        boost::lock_guard<boost::recursive_mutex> guard(*endp->getMutex());
        endp->getAttributes()->multicastLocatorList = mulList;
	}
	return valid;
}

/* Commented for now 
bool RTPSParticipantImpl::assignLocatorForBuiltin_unsafe(LocatorList_t& list, bool isMulti, bool isFixed)
{
		//Required for the built-in protocols
	bool valid = true;
	LocatorList_t finalList;
	bool added = false;
	for(auto lit = list.begin();lit != list.end();++lit)
	{
		added = false;
		for(std::vector<ListenResource*>::iterator it = m_listenResourceList.begin();it!=m_listenResourceList.end();++it)
		{
			if((*it)->isListeningTo(*lit))
			{
				LocatorList_t locList = (*it)->getListenLocators();
				finalList.push_back(locList);
				added = true;
			}
		}
		if(added)
			continue;
		ListenResource* LR = new ListenResource(this,++m_threadID,false);
		if(LR->init_thread(this,*lit,m_att.listenSocketBufferSize,isMulti,isFixed))
		{
			LocatorList_t locList = LR->getListenLocators();
			finalList.push_back(locList);
			m_listenResourceList.push_back(LR);
			added = true;
		}
		else
		{
			delete(LR);
			valid &= false;
		}
	}
	if(valid && added)
		list = finalList;
	return valid;
}
*/

bool RTPSParticipantImpl::createAndAssociateReceiverswithEndpoint(Endpoint * pend, bool isBuiltIn){
	/*	This function...
		- Asks the network factory for new resources
		- Encapsulates the new resources within the ReceiverControlBlock list
		- Associated the endpoint to the new elements in the list
		- Launches the listener thread
	*/
	// 1 - Ask the network factory to generate the elements that do still not exist
	std::vector<ReceiverResource> newItems;							//Store the newly created elements
	std::vector<ReceiverResource> newItemsBuffer;					//Store intermediate results
	//Iterate through the list of unicast and multicast locators the endpoint has... unless its empty
	//In that case, just use the standard
	if (pend->getAttributes()->unicastLocatorList.empty()){
		//Default unicast
		for (auto it = m_att.defaultUnicastLocatorList.begin(); it != m_att.defaultUnicastLocatorList.end(); ++it){
			newItemsBuffer = m_network_Factory.BuildReceiverResources((*it));
			newItems.insert(newItems.end(), newItemsBuffer.begin(), newItemsBuffer.end());
			newItemsBuffer.clear();
		}
	}else{
		//Endpoint unicast
		for (auto it = pend->getAttributes()->unicastLocatorList.begin(); it != pend->getAttributes()->unicastLocatorList.end(); ++it){
			newItemsBuffer = m_network_Factory.BuildReceiverResources((*it));
			newItems.insert(newItems.end(), newItemsBuffer.begin(), newItemsBuffer.end());
			newItemsBuffer.clear();
		}
	}
	if (pend->getAttributes()->multicastLocatorList.empty()){
		//Default multicast
		for (auto it = m_att.defaultMulticastLocatorList.begin(); it != m_att.defaultMulticastLocatorList.end(); ++it){
			newItemsBuffer = m_network_Factory.BuildReceiverResources((*it));
			newItems.insert(newItems.end(), newItemsBuffer.begin(), newItemsBuffer.end());
			newItemsBuffer.clear();
		}
	}else{
		//Endpoint multicast
		for (auto it = pend->getAttributes()->multicastLocatorList.begin(); it != pend->getAttributes()->multicastLocatorList.end(); ++it){
			newItemsBuffer = m_network_Factory.BuildReceiverResources((*it));
			newItems.insert(newItems.end(), newItemsBuffer.begin(), newItemsBuffer.end());
			newItemsBuffer.clear();
		}
	}
	// 2 - For each generated element...
	for (auto it = newItems.begin(); it != newItems.end(); ++it){
		// 2.1 - Initialize a ReceiverResourceControlBlock
		ReceiverControlBlock newBlock{ std::move((*it)), std::vector<RTPSWriter *>(), std::vector<RTPSReader *>(), nullptr, boost::mutex(), nullptr };
		newBlock.mp_receiver = new MessageReceiver(listenSockSize);			//!! listenSockSize has to come from somewhere 
		// 2.2 - Push it to the list
		m_receiverResourcelist.push_back(newBlock);
	}
	// 3 - Associate the Endpoint with ReceiverResources (all of them, not just the new)
	assignEndpointListenResources(pend,isBuiltIn); 

	// 4 - Launch the Listening thread for all of the uninitialized ReceiveResources
	for (auto it = m_receiverResourcelist.begin(); it != m_receiverResourcelist.end(); ++it){
		if ((*it)->mp_thread == nullptr)
				(*it)->mp_thread = new boost::thread(&RTPSParticipantImpl::performListenOperation, this, it);	//Bugfix
	}
	//note: Should the default locator list be updated with the creation of new ReceiveResources?
	return true;
}

void RTPSParticipantImpl::performListenOperation(ReceiverControlBlock *receiver){
	std::vector<char> localBuffer;
	Locator_t input_locator;
	//0 - Reset the buffer where the CDRMessage is going to be stored
	CDRMessage::initCDRMsg(&receiver->mp_receiver->m_rec_msg);
	//1 - Perform a blocking call to the receiver
	receiver->Receiver.Receive(localBuffer, input_locator);
	//2 - Output the data into struct's message receiver buffer
	for (int i = 0; i < localBuffer.size(); i++){
		receiver->mp_receiver->m_rec_msg.buffer = localBuffer.at(i);
	}
	receiver->mp_receiver->m_rec_msg.length = localBuffer.size();
	//3 - Call MessageReceiver methods.
		// The way this worked previously is the following: After receiving a message and putting it into the 
		// message receiver, ListenResource.newCDRMessage(), which then calls the message receiver, was called.
		// For the sake of fast integration, the funcionality of the newCRDMessage method is encapsulated here.
		// Then the call to the messageReceiver is going to stay the same (and everything after that).
		// Of course, since MessageReceiver was contained inside ListenResource and now it belongs to the Participant
		// itself, it has to be adapted to its new location

		//Since we already have the locator, there is no read need to perform any more operations

	//Call to  messageReceiver trigger function
	receiver->mp_receiver->processCDRMessage(mp_userParticipant->getGUID().guidprefix, &input_locator, &receiver->mp_receiver->m_rec_msg);
	//Call this function again
	performListenOperation(receiver);

}


bool RTPSParticipantImpl::assignEndpoint2LocatorList(Endpoint* endp,LocatorList_t& list,bool isMulti,bool isFixed)
{
	/* Note:
		The previous version of this function associated (or created) ListenResources and added the endpoint to them.
		It then requested the list of Locators the Listener is listening to and appended to the LocatorList_t from the paremeters.
		
		This has been removed becuase it is considered redundant. For ReceiveResources that listen on multiple interfaces, only
		one of the supported Locators is needed to make the match, and the case of new ListenResources being created has been removed
		since its the NetworkFactory the one that takes care of Resource creation.
	 */
	bool found = false;
	LocatorList_t finalList;
	for(auto lit = list.begin();lit != list.end();++lit){
		//Iteration of all Locators within the Locator list passed down as argument
		boost::lock_guard<boost::recursive_mutex> guard(*mp_mutex);
		//Check among ReceiverResources whether the locator is supported or not
		for (auto it = m_receiverResourcelist.begin(); it != m_receiverResourcelist.end(); ++it){
			if ((*it)->Receiver->SupportsLocator(*lit)){
				//Supported! Take mutex and update lists - We maintain reader/writer discrimination just in case
				found = false;
				if (endp->getAttributes()->endpointKind == WRITER){
					for (std::vector<RTPSWriter*>::iterator wit = (*it)->AssociatedWriters.begin(); wit != (*it)->AssociatedWriters.end(); ++wit){
						if ((*wit)->getGuid().entityId == endp->getGuid().entityId){
							found = true;
							break;
						}
					}
					//After iterating among associated writers, add the new writer if it has not been found
					if (!found){
						(*it)->AssociatedWriters.push_back((RTPSWriter*)endp);
						return true;
					}
				}
				else if (endp->getAttributes()->endpointKind == READER){
					for (std::vector<RTPSReader*>::iterator rit = (*it)->AssociatedReaders.begin(); rit != (*it)->AssociatedReaders.end(); ++rit){
						if ((*rit)->getGuid().entityId == endp->getGuid().entityId){
							found = true;
							break;
						}
					}
					if (!found){
						(*it)->AssociatedReaders.push_back((RTPSReader*)endp);
						return true;
					}
				}
				// end association between reader/writer and the receive resources
			}
		}
		//Finished iteratig through all ListenResources for a single Locator (from the parameter list).
		//Since this function is called after checking with NetFactory we do not have to create any more resource. 
	}
	return true;
}
bool RTPSParticipantImpl::createSendResources(Endpoint *pend){
	std::vector<SenderResource> newSenders;
	std::vector<SenderResource> SendersBuffer;
	if (pend->m_att.outLocatorList.empty()){
		//Output locator ist is empty, use predetermined ones
		for (auto it = m_att.defaultOutLocatorList.begin(); it != m.att.defaultOutLocatorList.end(); ++i){
			SendersBuffer = m_network_Factory.BuildSenderResources((*it));
			newSenders.insert(newSenders.end(), SendersBuffer.begin(), SendersBuffer.end());
			SendersBuffer.clear();
		}
	}
	else{
		//Output locators have been specified, create them
		for (auto it = pend->m_att.outLocatorList.begin(); it != pend->m_att.outLocatorList.end(); ++it){
			SendersBuffer = m_network_Factory.BuildSenderResources((*it));
			newSenders.insert(newSenders.end(), SendersBuffer.begin(), SendersBuffer.end());
			SendersBuffer.clear();
		}
	}
	m_senderResource.insert(m_senderResource.end(), SendersBuffer.begin(), SendersBuffer.end());

	return true;
}

bool RTPSParticipantImpl::deleteUserEndpoint(Endpoint* p_endpoint)
{
	bool found = false;
	{

		if(p_endpoint->getAttributes()->endpointKind == WRITER)
		{
			boost::lock_guard<boost::recursive_mutex> guard(*mp_mutex);
			for(auto wit=m_userWriterList.begin();
					wit!=m_userWriterList.end();++wit)
			{
				if((*wit)->getGuid().entityId == p_endpoint->getGuid().entityId) //Found it
				{
					m_userWriterList.erase(wit);
					found = true;
					break;
				}
			}
		}
		else
		{
			boost::lock_guard<boost::recursive_mutex> guard(*mp_mutex);
			for(auto rit=m_userReaderList.begin()
					;rit!=m_userReaderList.end();++rit)
			{
				if((*rit)->getGuid().entityId == p_endpoint->getGuid().entityId) //Found it
				{
					m_userReaderList.erase(rit);
					found = true;
					break;
				}
			}
		}
		if(!found)
			return false;
		//REMOVE FOR BUILTINPROTOCOLS
		if(p_endpoint->getAttributes()->endpointKind == WRITER)
			mp_builtinProtocols->removeLocalWriter((RTPSWriter*)p_endpoint);
		else
			mp_builtinProtocols->removeLocalReader((RTPSReader*)p_endpoint);
		//BUILTINPROTOCOLS
		//Remove it from threadListenList
		std::vector<ListenResource*>::iterator thit;
		for(thit=m_listenResourceList.begin();
				thit!=m_listenResourceList.end();thit++)
		{
			(*thit)->removeAssociatedEndpoint(p_endpoint);
		}
		boost::lock_guard<boost::recursive_mutex> guardParticipant(*mp_mutex);
		bool continue_removing = true;
		while(continue_removing)
		{
			continue_removing = false;
			for(thit=m_listenResourceList.begin();
					thit!=m_listenResourceList.end();thit++)
			{
				if(!(*thit)->hasAssociatedEndpoints() && ! (*thit)->m_isDefaultListenResource)
				{
					delete(*thit);
					m_listenResourceList.erase(thit);
					continue_removing = true;
					break;
				}
			}
		}
	}
	//	boost::lock_guard<boost::recursive_mutex> guardEndpoint(*p_endpoint->getMutex());
	delete(p_endpoint);
	return true;
}


ResourceEvent& RTPSParticipantImpl::getEventResource()
{
	return *this->mp_event_thr;
}

void RTPSParticipantImpl::sendSync(CDRMessage_t* msg, Endpoint *pend, const Locator_t& destination_loc)
{
	//Translate data into standard contained and send
	std::vector<char> buffer;
	for (i = 0; i < msg->length; i++){
		buffer.insert(msg->buffer[i]);
	}
	for (auto sit = pend->m_att.outLocatorList.begin(); sit != pend->m_att.outLocatorList.end(); ++sit){
		for (auto it = m_senderResource.begin(); it != m_senderResource.end(); ++it){
			if ((*it)->SupportsLocator((*sit)){
				(*it)->Send(buffer, destination_loc);
			}
		}
	}

	return;
}

void RTPSParticipantImpl::announceRTPSParticipantState()
{
	return mp_builtinProtocols->announceRTPSParticipantState();
}

void RTPSParticipantImpl::stopRTPSParticipantAnnouncement()
{
	return mp_builtinProtocols->stopRTPSParticipantAnnouncement();
}

void RTPSParticipantImpl::resetRTPSParticipantAnnouncement()
{
	return mp_builtinProtocols->resetRTPSParticipantAnnouncement();
}

void RTPSParticipantImpl::loose_next_change()
{
	this->mp_send_thr->loose_next_change();
}


bool RTPSParticipantImpl::newRemoteEndpointDiscovered(const GUID_t& pguid, int16_t userDefinedId,EndpointKind_t kind)
{
	const char* const METHOD_NAME = "newRemoteEndpointDiscovered";
	if(m_att.builtin.use_STATIC_EndpointDiscoveryProtocol == false)
	{
		logWarning(RTPS_PARTICIPANT,"Remote Endpoints can only be activated with static discovery protocol");
		return false;
	}
	return mp_builtinProtocols->mp_PDP->newRemoteEndpointStaticallyDiscovered(pguid,userDefinedId,kind);
}

void RTPSParticipantImpl::ResourceSemaphorePost()
{
	if(mp_ResourceSemaphore != nullptr)
	{
		mp_ResourceSemaphore->post();
	}
}

void RTPSParticipantImpl::ResourceSemaphoreWait()
{
	if (mp_ResourceSemaphore != nullptr)
	{
		mp_ResourceSemaphore->wait();
	}

}

boost::recursive_mutex* RTPSParticipantImpl::getSendMutex()
{
	return mp_send_thr->getMutex();
}

void RTPSParticipantImpl::assertRemoteRTPSParticipantLiveliness(const GuidPrefix_t& guidP)
{
	this->mp_builtinProtocols->mp_PDP->assertRemoteParticipantLiveliness(guidP);
}



}
} /* namespace rtps */
} /* namespace eprosima */


