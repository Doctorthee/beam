// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "node.h"
#include "../core/serialization_adapters.h"
#include "../core/proto.h"
#include "../core/ecc_native.h"

#include "../p2p/protocol.h"
#include "../p2p/connection.h"

#include "../utility/io/tcpserver.h"
#include "../utility/logger.h"
#include "../utility/logger_checkpoints.h"

namespace beam {

void Node::RefreshCongestions()
{
	if (m_pSync)
		return;

	for (TaskSet::iterator it = m_setTasks.begin(); m_setTasks.end() != it; it++)
		it->m_bRelevant = false;

	m_Processor.EnumCongestions();

	for (TaskList::iterator it = m_lstTasksUnassigned.begin(); m_lstTasksUnassigned.end() != it; )
	{
		Task& t = *(it++);
		if (!t.m_bRelevant)
			DeleteUnassignedTask(t);
	}
}

void Node::DeleteUnassignedTask(Task& t)
{
	assert(!t.m_pOwner && !t.m_bPack);
	m_lstTasksUnassigned.erase(TaskList::s_iterator_to(t));
	m_setTasks.erase(TaskSet::s_iterator_to(t));
	delete &t;
}

uint32_t Node::WantedTx::get_Timeout_ms()
{
	return get_ParentObj().m_Cfg.m_Timeout.m_GetTx_ms;
}

void Node::WantedTx::OnExpired(const KeyType& key)
{
	proto::GetTransaction msg;
	msg.m_ID = key;

	for (PeerList::iterator it = get_ParentObj().m_lstPeers.begin(); get_ParentObj().m_lstPeers.end() != it; it++)
	{
		Peer& peer = *it;
		if (peer.m_Config.m_SpreadingTransactions)
			peer.Send(msg);
	}
}

void Node::Bbs::CalcMsgKey(NodeDB::WalkerBbs::Data& d)
{
	ECC::Hash::Processor()
		<< d.m_Message
		<< d.m_Channel
		>> d.m_Key;
}

uint32_t Node::Bbs::WantedMsg::get_Timeout_ms()
{
	return get_ParentObj().get_ParentObj().m_Cfg.m_Timeout.m_GetBbsMsg_ms;
}

void Node::Bbs::WantedMsg::OnExpired(const KeyType& key)
{
	proto::BbsGetMsg msg;
	msg.m_Key = key;

	for (PeerList::iterator it = get_ParentObj().get_ParentObj().m_lstPeers.begin(); get_ParentObj().get_ParentObj().m_lstPeers.end() != it; it++)
	{
		Peer& peer = *it;
		if (peer.m_Config.m_Bbs)
			peer.Send(msg);
	}

	get_ParentObj().MaybeCleanup();
}

void Node::Wanted::Clear()
{
	while (!m_lst.empty())
		DeleteInternal(m_lst.back());
}

void Node::Wanted::DeleteInternal(Item& n)
{
	m_lst.erase(List::s_iterator_to(n));
	m_set.erase(Set::s_iterator_to(n));
	delete &n;
}

void Node::Wanted::Delete(Item& n)
{
	bool bFront = (&m_lst.front() == &n);
	DeleteInternal(n);

	if (bFront)
		SetTimer();
}

bool Node::Wanted::Delete(const KeyType& key)
{
	Item n;
	n.m_Key = key;

	Set::iterator it = m_set.find(n);
	if (m_set.end() == it)
		return false;

	Delete(*it);
	return true;
}

bool Node::Wanted::Add(const KeyType& key)
{
	Item n;
	n.m_Key = key;
	Set::iterator it = m_set.find(n);
	if (m_set.end() != it)
		return false; // already waiting for it

	bool bEmpty = m_lst.empty();

	Item* p = new Item;
	p->m_Key = key;
	p->m_Advertised_ms = GetTime_ms();

	m_set.insert(*p);
	m_lst.push_back(*p);

	if (bEmpty)
		SetTimer();

	return true;
}

void Node::Wanted::SetTimer()
{
	if (m_lst.empty())
	{
		if (m_pTimer)
			m_pTimer->cancel();
	}
	else
	{
		if (!m_pTimer)
			m_pTimer = io::Timer::create(io::Reactor::get_Current());

		uint32_t dt = GetTime_ms() - m_lst.front().m_Advertised_ms;
		const uint32_t timeout_ms = get_Timeout_ms();

		m_pTimer->start((timeout_ms > dt) ? (timeout_ms - dt) : 0, false, [this]() { OnTimer(); });
	}
}

void Node::Wanted::OnTimer()
{
	uint32_t t_ms = GetTime_ms();
	const uint32_t timeout_ms = get_Timeout_ms();

	while (!m_lst.empty())
	{
		Item& n = m_lst.front();
		if (t_ms - n.m_Advertised_ms < timeout_ms)
			break;

		OnExpired(n.m_Key); // should not invalidate our structure
		Delete(n); // will also reschedule the timer
	}
}

void Node::TryAssignTask(Task& t, const PeerID* pPeerID)
{
	while (true)
	{
		Peer* pSel = NULL;

		if (pPeerID)
		{
			bool bCreate = false;
			PeerMan::PeerInfoPlus* pInfo = (PeerMan::PeerInfoPlus*) m_PeerMan.Find(*pPeerID, bCreate);

			if (pInfo && pInfo->m_pLive && (Peer::Flags::PiRcvd & pInfo->m_pLive->m_Flags))
				pSel = pInfo->m_pLive;
		}

		for (PeerList::iterator it = m_lstPeers.begin(); !pSel && (m_lstPeers.end() != it); it++)
		{
			Peer& p = *it;
			if (ShouldAssignTask(t, p))
			{
				pSel = &p;
				break;
			}
		}

		if (!pSel)
			break;

		try {
			AssignTask(t, *pSel);
			return; // done
		}
		catch (const std::exception& e) {
			pSel->OnExc(e);
		}

		//  retry
	}
}

void Node::AssignTask(Task& t, Peer& p)
{
	uint32_t nPackSize = 0;
	if (t.m_Key.first.m_Height > m_Processor.m_Cursor.m_ID.m_Height)
	{
		Height dh = t.m_Key.first.m_Height - m_Processor.m_Cursor.m_ID.m_Height;

		const uint32_t nThreshold = 5;

		if (dh >= nThreshold)
		{
			nPackSize = proto::g_HdrPackMaxSize;
			if (nPackSize > dh)
				nPackSize = (uint32_t) dh;
		}
	}

	if (t.m_Key.second)
	{
		proto::GetBody msg;
		msg.m_ID = t.m_Key.first;
		p.Send(msg);
	}
	else
	{
		if (!m_nTasksPackHdr && nPackSize)
		{
			proto::GetHdrPack msg;
			msg.m_Top = t.m_Key.first;
			msg.m_Count = nPackSize;
			p.Send(msg);

			t.m_bPack = true;
			m_nTasksPackHdr++;
		}
		else
		{
			proto::GetHdr msg;
			msg.m_ID = t.m_Key.first;
			p.Send(msg);
		}
	}

	bool bEmpty = p.m_lstTasks.empty();

	assert(!t.m_pOwner);
	t.m_pOwner = &p;

	m_lstTasksUnassigned.erase(TaskList::s_iterator_to(t));
	p.m_lstTasks.push_back(t);

	if (bEmpty)
		p.SetTimerWrtFirstTask();
}

void Node::Peer::SetTimerWrtFirstTask()
{
	if (m_lstTasks.empty())
		KillTimer();
	else
		SetTimer(m_lstTasks.front().m_Key.second ? m_This.m_Cfg.m_Timeout.m_GetBlock_ms : m_This.m_Cfg.m_Timeout.m_GetState_ms);
}

bool Node::ShouldAssignTask(Task& t, Peer& p)
{
	if (p.m_Tip.m_Height < t.m_Key.first.m_Height)
		return false;

	if (p.m_Tip.m_Height == t.m_Key.first.m_Height)
	{
		Merkle::Hash hv;
		p.m_Tip.get_Hash(hv);

		if (hv != t.m_Key.first.m_Hash)
			return false;
	}

	// Current design: don't ask anything from non-authenticated peers
	if (!((Peer::Flags::PiRcvd & p.m_Flags) && p.m_pInfo))
		return false;

	// check if the peer currently transfers a block
	for (TaskList::iterator it = p.m_lstTasks.begin(); p.m_lstTasks.end() != it; it++)
		if (it->m_Key.second)
			return false;

	return p.m_setRejected.end() == p.m_setRejected.find(t.m_Key);
}

void Node::Processor::RequestData(const Block::SystemState::ID& id, bool bBlock, const PeerID* pPreferredPeer)
{
	Task tKey;
	tKey.m_Key.first = id;
	tKey.m_Key.second = bBlock;

	TaskSet::iterator it = get_ParentObj().m_setTasks.find(tKey);
	if (get_ParentObj().m_setTasks.end() == it)
	{
		LOG_INFO() << "Requesting " << (bBlock ? "block" : "header") << " " << id;

		Task* pTask = new Task;
		pTask->m_Key = tKey.m_Key;
		pTask->m_bRelevant = true;
		pTask->m_bPack = false;
		pTask->m_pOwner = NULL;

		get_ParentObj().m_setTasks.insert(*pTask);
		get_ParentObj().m_lstTasksUnassigned.push_back(*pTask);

		get_ParentObj().TryAssignTask(*pTask, pPreferredPeer);


		int diff = static_cast<int>(id.m_Height - m_Cursor.m_ID.m_Height);
		m_RequestedCount = std::max(m_RequestedCount, diff);

		ReportProgress();

	} else
		it->m_bRelevant = true;
}

void Node::Processor::OnPeerInsane(const PeerID& peerID)
{
	bool bCreate = false;
	PeerMan::PeerInfoPlus* pInfo = (PeerMan::PeerInfoPlus*) get_ParentObj().m_PeerMan.Find(peerID, bCreate);

	if (pInfo)
	{
		if (pInfo->m_pLive)
			pInfo->m_pLive->DeleteSelf(true, proto::NodeConnection::ByeReason::Ban);
		else
			get_ParentObj().m_PeerMan.Ban(*pInfo);
	}
}

void Node::Processor::OnNewState()
{
	m_Cwp.Reset();

	if (!m_Cursor.m_Sid.m_Row)
		return;

	LOG_INFO() << "My Tip: " << m_Cursor.m_ID;

	get_ParentObj().m_TxPool.DeleteOutOfBound(m_Cursor.m_Sid.m_Height + 1);

	get_ParentObj().m_Miner.HardAbortSafe();

	get_ParentObj().m_Miner.SetTimer(0, true); // don't start mined block construction, because we're called in the context of NodeProcessor, which holds the DB transaction.

	proto::NewTip msg;
	msg.m_Description = m_Cursor.m_Full;

	for (PeerList::iterator it = get_ParentObj().m_lstPeers.begin(); get_ParentObj().m_lstPeers.end() != it; it++)
	{
		Peer& peer = *it;
		if (!(Peer::Flags::Connected & peer.m_Flags))
			continue;

		if (!NodeProcessor::IsRemoteTipNeeded(msg.m_Description, peer.m_Tip))
			continue;

		peer.Send(msg);
	}

	get_ParentObj().m_Compressor.OnNewState();

	get_ParentObj().RefreshCongestions();

    ReportNewState();
}

void Node::Processor::OnRolledBack()
{
	LOG_INFO() << "Rolled back to: " << m_Cursor.m_ID;
	get_ParentObj().m_Compressor.OnRolledBack();
}

bool Node::Processor::VerifyBlock(const Block::BodyBase& block, TxBase::IReader&& r, const HeightRange& hr)
{
	uint32_t nThreads = get_ParentObj().m_Cfg.m_VerificationThreads;
	if (!nThreads)
	{
		std::unique_ptr<Verifier::MyBatch> p(new Verifier::MyBatch);
		p->m_bEnableBatch = true;
		Verifier::MyBatch::Scope scope(*p);

		return
			NodeProcessor::VerifyBlock(block, std::move(r), hr) &&
			p->Flush();
	}

	Verifier& v = m_Verifier; // alias
	std::unique_lock<std::mutex> scope(v.m_Mutex);

	if (v.m_vThreads.empty())
	{
		v.m_iTask = 1;

		v.m_vThreads.resize(nThreads);
		for (uint32_t i = 0; i < nThreads; i++)
			v.m_vThreads[i] = std::thread(&Verifier::Thread, &v, i);
	}


	v.m_iTask ^= 2;
	v.m_pTx = &block;
	v.m_pR = &r;
	v.m_bFail = false;
	v.m_Remaining = nThreads;
	v.m_Context.m_bBlockMode = true;
	v.m_Context.m_Height = hr;
	v.m_Context.m_nVerifiers = nThreads;

	v.m_TaskNew.notify_all();

	while (v.m_Remaining)
		v.m_TaskFinished.wait(scope);

	return !v.m_bFail && v.m_Context.IsValidBlock(block, m_Extra.m_SubsidyOpen);
}

void Node::Processor::Verifier::Thread(uint32_t iVerifier)
{
	std::unique_ptr<Verifier::MyBatch> p(new Verifier::MyBatch);
	p->m_bEnableBatch = true;
	Verifier::MyBatch::Scope scope(*p);

	for (uint32_t iTask = 1; ; )
	{
		{
			std::unique_lock<std::mutex> scope2(m_Mutex);

			while (m_iTask == iTask)
				m_TaskNew.wait(scope2);

			if (!m_iTask)
				return;

			iTask = m_iTask;
		}

		p->Reset();

		assert(m_Remaining);

		TxBase::Context ctx;
		ctx.m_bBlockMode = true;
		ctx.m_Height = m_Context.m_Height;
		ctx.m_nVerifiers = m_Context.m_nVerifiers;
		ctx.m_iVerifier = iVerifier;
		ctx.m_pAbort = &m_bFail; // obsolete actually

		TxBase::IReader::Ptr pR;
		m_pR->Clone(pR);

		bool bValid = ctx.ValidateAndSummarize(*m_pTx, std::move(*pR)) && p->Flush();

		std::unique_lock<std::mutex> scope2(m_Mutex);

		verify(m_Remaining--);

		if (bValid && !m_bFail)
			bValid = m_Context.Merge(ctx);

		if (!bValid)
			m_bFail = true;

		if (!m_Remaining)
			m_TaskFinished.notify_one();
	}
}

bool Node::Processor::ApproveState(const Block::SystemState::ID& id)
{
	const Block::SystemState::ID& idCtl = get_ParentObj().m_Cfg.m_ControlState;
	return
		(idCtl.m_Height != id.m_Height) ||
		(idCtl.m_Hash == id.m_Hash);
}

void Node::Processor::AdjustFossilEnd(Height& h)
{
	if (get_ParentObj().m_Compressor.m_bEnabled)
	{
		// blocks above the oldest macroblock should be accessuble
		NodeDB::WalkerState ws(get_DB());
		for (get_DB().EnumMacroblocks(ws); ws.MoveNext(); )
			if (h > ws.m_Sid.m_Height)
				h = ws.m_Sid.m_Height;
	}
}

void Node::Processor::OnStateData()
{
	++m_DownloadedHeaders;
	ReportProgress();
}

void Node::Processor::OnBlockData()
{
	++m_DownloadedBlocks;
	ReportProgress();
}

bool Node::Processor::OpenMacroblock(Block::BodyBase::RW& rw, const NodeDB::StateID& sid)
{
	get_ParentObj().m_Compressor.FmtPath(rw, sid.m_Height, NULL);
	rw.ROpen();
	return true;
}

void Node::Processor::ReportProgress()
{
	auto observer = get_ParentObj().m_Cfg.m_Observer;
	if (observer)
	{
		int total = m_RequestedCount * 2;
		int done = m_DownloadedHeaders + m_DownloadedBlocks;
		if (total >= done)
		{
			observer->OnSyncProgress(done, total);
		}

		if (done >= total)
		{
			m_RequestedCount = m_DownloadedHeaders = m_DownloadedBlocks;
		}
	}
}

void Node::Processor::ReportNewState()
{
    auto observer = get_ParentObj().m_Cfg.m_Observer;
	if (observer)
    {
        observer->OnStateChanged();
    }
}

Node::Peer* Node::AllocPeer(const beam::io::Address& addr)
{
	Peer* pPeer = new Peer(*this);
	m_lstPeers.push_back(*pPeer);

	pPeer->m_pInfo = NULL;
	pPeer->m_Flags = 0;
	pPeer->m_Port = 0;
	ZeroObject(pPeer->m_Tip);
	pPeer->m_RemoteAddr = addr;
	ZeroObject(pPeer->m_Config);

	LOG_INFO() << "+Peer " << addr;

	return pPeer;
}

void Node::Initialize()
{
	m_Processor.m_Horizon = m_Cfg.m_Horizon;
	m_Processor.Initialize(m_Cfg.m_sPathLocal.c_str(), m_Cfg.m_Sync.m_ForceResync);
	m_Processor.m_Kdf.m_Secret = m_Cfg.m_WalletKey;

	if (m_Cfg.m_Sync.m_ForceResync)
		m_Processor.get_DB().ParamSet(NodeDB::ParamID::SyncTarget, NULL, NULL);

	if (m_Cfg.m_VerificationThreads < 0)
	{
		uint32_t numCores = std::thread::hardware_concurrency();
		m_Cfg.m_VerificationThreads = (numCores > m_Cfg.m_MiningThreads + 1) ? (numCores - m_Cfg.m_MiningThreads) : 0;
	}

	InitIDs();

	LOG_INFO() << "Node ID=" << m_MyPublicID << ", Owner=" << m_MyOwnerID;
	LOG_INFO() << "Initial Tip: " << m_Processor.m_Cursor.m_ID;

	InitMode();

	RefreshCongestions();

	if (m_Cfg.m_Listen.port())
	{
		m_Server.Listen(m_Cfg.m_Listen);
		if (m_Cfg.m_BeaconPeriod_ms)
			m_Beacon.Start();
	}

	m_PeerMan.Initialize();
	m_Miner.Initialize();
	m_Compressor.Init();
	m_Bbs.Cleanup();
}

void Node::InitIDs()
{
	ECC::GenRandom(m_NonceLast.V.m_pData, m_NonceLast.V.nBytes);

	m_MyPrivateID.V.m_Value = Zero;

	Blob blob(m_MyPrivateID.V.m_Value);
	bool bNewID = !m_Processor.get_DB().ParamGet(NodeDB::ParamID::MyID, NULL, &blob);

	if (bNewID)
	{
		ECC::Scalar::Native sk;
		NextNonce(sk);
		m_MyPrivateID.V = sk;
	}

	ECC::Scalar::Native sk = m_MyPrivateID.V;
	proto::Sk2Pk(m_MyPublicID, sk);

	if (bNewID)
	{
		m_MyPrivateID.V = sk; // may have been negated
		m_Processor.get_DB().ParamSet(NodeDB::ParamID::MyID, NULL, &blob);
	}

	DeriveKey(sk, m_Processor.m_Kdf, 0, KeyType::Identity);
	proto::Sk2Pk(m_MyOwnerID, sk);
}

void Node::InitMode()
{
	if (m_Processor.m_Cursor.m_ID.m_Height)
		return;

	if (!m_Cfg.m_vTreasury.empty())
	{
		LOG_INFO() << "Creating new blockchain from treasury";
		return;
	}

	if (!m_Cfg.m_Sync.m_SrcPeers)
		return;

	LOG_INFO() << "Sync mode";

	m_pSync.reset(new FirstTimeSync);
	ZeroObject(m_pSync->m_Trg);
	ZeroObject(m_pSync->m_Best);

	Blob blobTrg(m_pSync->m_Trg.m_Hash);
	m_Processor.get_DB().ParamGet(NodeDB::ParamID::SyncTarget, &m_pSync->m_Trg.m_Height, &blobTrg);

	m_pSync->m_bDetecting = !m_pSync->m_Trg.m_Height;

	if (m_pSync->m_Trg.m_Height)
	{
		LOG_INFO() << "Resuming sync up to " << m_pSync->m_Trg;
	}
	else
	{
		LOG_INFO() << "Searching for the best peer...";
	}
}

void Node::Bbs::Cleanup()
{
	get_ParentObj().m_Processor.get_DB().BbsDelOld(getTimestamp() - get_ParentObj().m_Cfg.m_Timeout.m_BbsMessageTimeout_s);
	m_LastCleanup_ms = GetTime_ms();

	FindRecommendedChannel();
}

void Node::Bbs::FindRecommendedChannel()
{
	NodeDB& db = get_ParentObj().m_Processor.get_DB(); // alias

	BbsChannel nChannel = 0;
	uint32_t nCount = 0, nCountFound = 0;
	bool bFound = false;

	NodeDB::WalkerBbs wlk(db);
	for (db.EnumAllBbs(wlk); ; )
	{
		bool bMoved = wlk.MoveNext();

		if (bMoved && (wlk.m_Data.m_Channel == nChannel))
			nCount++;
		else
		{
			if ((nCount <= get_ParentObj().m_Cfg.m_BbsIdealChannelPopulation) && (!bFound || (nCountFound < nCount)))
			{
				bFound = true;
				nCountFound = nCount;
				m_RecommendedChannel = nChannel;
			}

			if (!bFound && (nChannel + 1 != wlk.m_Data.m_Channel)) // fine also for !bMoved
			{
				bFound = true;
				nCountFound = 0;
				m_RecommendedChannel = nChannel + 1;
			}

			if (!bMoved)
				break;

			nChannel = wlk.m_Data.m_Channel;
			nCount = 1;
		}
	}

	assert(bFound);
}

void Node::Bbs::MaybeCleanup()
{
	uint32_t dt_ms = GetTime_ms() - m_LastCleanup_ms;
	if (dt_ms >= get_ParentObj().m_Cfg.m_Timeout.m_BbsCleanupPeriod_ms)
		Cleanup();
}

void Node::ImportMacroblock(Height h)
{
	Block::BodyBase::RW rw;
	m_Compressor.FmtPath(rw, h, NULL);
	rw.ROpen();

	if (!m_Processor.ImportMacroBlock(rw))
		throw std::runtime_error("import failed");

	if (m_Processor.m_Cursor.m_Sid.m_Row)
		m_Processor.get_DB().MacroblockIns(m_Processor.m_Cursor.m_Sid.m_Row);
}

Node::~Node()
{
	LOG_INFO() << "Node stopping...";

	m_Miner.HardAbortSafe();

	for (size_t i = 0; i < m_Miner.m_vThreads.size(); i++)
	{
		PerThread& pt = m_Miner.m_vThreads[i];
		if (pt.m_pReactor)
			pt.m_pReactor->stop();

		if (pt.m_Thread.joinable())
			pt.m_Thread.join();
	}
	m_Miner.m_vThreads.clear();

	m_Compressor.StopCurrent();

	for (PeerList::iterator it = m_lstPeers.begin(); m_lstPeers.end() != it; it++)
		ZeroObject(it->m_Config); // prevent re-assigning of tasks in the next loop

	m_pSync = NULL; // prevent reassign sync

	while (!m_lstPeers.empty())
		m_lstPeers.front().DeleteSelf(false, proto::NodeConnection::ByeReason::Stopping);

	while (!m_lstTasksUnassigned.empty())
		DeleteUnassignedTask(m_lstTasksUnassigned.front());

	assert(m_setTasks.empty());

	Processor::Verifier& v = m_Processor.m_Verifier; // alias
	if (!v.m_vThreads.empty())
	{
		{
			std::unique_lock<std::mutex> scope(v.m_Mutex);
			v.m_iTask = 0;
			v.m_TaskNew.notify_all();
		}

		for (size_t i = 0; i < v.m_vThreads.size(); i++)
			if (v.m_vThreads[i].joinable())
				v.m_vThreads[i].join();
	}

	LOG_INFO() << "Node stopped";
}

void Node::Peer::SetTimer(uint32_t timeout_ms)
{
	if (!m_pTimer)
		m_pTimer = io::Timer::create(io::Reactor::get_Current());

	m_pTimer->start(timeout_ms, false, [this]() { OnTimer(); });
}

void Node::Peer::KillTimer()
{
	assert(m_pTimer);
	m_pTimer->cancel();
}

void Node::Peer::OnTimer()
{
	if (Flags::Connected & m_Flags)
	{
		assert(!m_lstTasks.empty());

		LOG_WARNING() << "Peer " << m_RemoteAddr << " request timeout";

		if (m_pInfo)
			m_This.m_PeerMan.ModifyRating(*m_pInfo, PeerMan::Rating::PenaltyTimeout, false); // task (request) wasn't handled in time.

		DeleteSelf(false, ByeReason::Timeout);
	}
	else
		// Connect didn't finish in time
		DeleteSelf(true, 0);
}

void Node::Peer::OnResendPeers()
{
	PeerMan& pm = m_This.m_PeerMan;
	const PeerMan::RawRatingSet& rs = pm.get_Ratings();
	uint32_t nRemaining = pm.m_Cfg.m_DesiredHighest;

	for (PeerMan::RawRatingSet::const_iterator it = rs.begin(); nRemaining && (rs.end() != it); it++)
	{
		const PeerMan::PeerInfo& pi = it->get_ParentObj();
		if ((Flags::PiRcvd & m_Flags) && (&pi == m_pInfo))
			continue; // skip

		proto::PeerInfo msg;
		msg.m_ID = pi.m_ID.m_Key;
		msg.m_LastAddr = pi.m_Addr.m_Value;
		Send(msg);
	}
}

void Node::Peer::GenerateSChannelNonce(ECC::Scalar::Native& nonce)
{
	m_This.NextNonce(nonce);
}

void Node::Peer::OnConnectedSecure()
{
	LOG_INFO() << "Peer " << m_RemoteAddr << " Connected";

	m_Flags |= Flags::Connected;

	if (m_Port && m_This.m_Cfg.m_Listen.port())
	{
		// we've connected to the peer, let it now know our port
		proto::PeerInfoSelf msgPi;
		msgPi.m_Port = m_This.m_Cfg.m_Listen.port();
		Send(msgPi);
	}

	ECC::Scalar::Native sk = m_This.m_MyPrivateID.V;
	ProveID(sk, proto::IDType::Node);

	proto::Config msgCfg;
	msgCfg.m_CfgChecksum = Rules::get().Checksum;
	msgCfg.m_SpreadingTransactions = true;
	msgCfg.m_Bbs = true;
	msgCfg.m_SendPeers = true;
	Send(msgCfg);

	if (m_This.m_Processor.m_Cursor.m_Sid.m_Row)
	{
		proto::NewTip msg;
		msg.m_Description = m_This.m_Processor.m_Cursor.m_Full;
		Send(msg);
	}
}

void Node::Peer::OnMsg(proto::Authentication&& msg)
{
	proto::NodeConnection::OnMsg(std::move(msg));
	LOG_INFO() << "Peer " << m_RemoteAddr << " Auth. Type=" << msg.m_IDType << ", ID=" << msg.m_ID;

	if (proto::IDType::Owner == msg.m_IDType)
	{
		if (msg.m_ID == m_This.m_MyOwnerID)
			m_Flags |= Flags::Owner;
	}

	if (proto::IDType::Node != msg.m_IDType)
		return;

	if ((Flags::PiRcvd & m_Flags) || (msg.m_ID == Zero))
		ThrowUnexpected();

	m_Flags |= Flags::PiRcvd;
	LOG_INFO() << m_RemoteAddr << " received PI";

	PeerMan& pm = m_This.m_PeerMan; // alias

	if (m_pInfo)
	{
		// probably we connected by the address
		if (m_pInfo->m_ID.m_Key == msg.m_ID)
		{
			pm.OnSeen(*m_pInfo);
			return; // all settled (already)
		}

		// detach from it
		m_pInfo->m_pLive = NULL;

		if (m_pInfo->m_ID.m_Key == Zero)
		{
			LOG_INFO() << "deleted anonymous PI";
			pm.Delete(*m_pInfo); // it's anonymous.
		}
		else
		{
			LOG_INFO() << "PeerID is different";
			pm.OnActive(*m_pInfo, false);
			pm.RemoveAddr(*m_pInfo); // turned-out to be wrong
		}

		m_pInfo = NULL;
	}

	if (msg.m_ID == m_This.m_MyPublicID)
	{
		LOG_WARNING() << "Loopback connection";
		DeleteSelf(false, ByeReason::Loopback);
		return;
	}

	io::Address addr;

	bool bAddrValid = (m_Port > 0);
	if (bAddrValid)
	{
		addr = m_RemoteAddr;
		addr.port(m_Port);
	} else
		LOG_INFO() << "No PI port"; // doesn't accept incoming connections?


	PeerMan::PeerInfoPlus* pPi = (PeerMan::PeerInfoPlus*) pm.OnPeer(msg.m_ID, addr, bAddrValid);
	assert(pPi);

	if (pPi->m_pLive)
	{
		LOG_INFO() << "Duplicate connection with the same PI.";
		// Duplicate connection. In this case we have to choose wether to terminate this connection, or the previous. The best is to do it asymmetrically.
		// We decide this based on our Node IDs.

		if (m_This.m_MyPublicID > msg.m_ID)
		{
			pPi->m_pLive->DeleteSelf(false, ByeReason::Duplicate);
			assert(!pPi->m_pLive);
		}
		else
		{
			DeleteSelf(false, ByeReason::Duplicate);
			return;
		}
	}

	if (!pPi->m_RawRating.m_Value)
	{
		LOG_INFO() << "Banned PI. Ignoring";
		DeleteSelf(false, ByeReason::Ban);
		return;
	}

	// attach to it
	pPi->m_pLive = this;
	m_pInfo = pPi;
	pm.OnActive(*pPi, true);
	pm.OnSeen(*pPi);

	LOG_INFO() << *m_pInfo << " connected, info updated";
}

void Node::Peer::OnDisconnect(const DisconnectReason& dr)
{
	LOG_WARNING() << m_RemoteAddr << ": " << dr;

	bool bIsErr = true;
	uint8_t nByeReason = 0;

	switch (dr.m_Type)
	{
	default: assert(false);

	case DisconnectReason::Io:
		break;

	case DisconnectReason::Bye:
		bIsErr = false;
		break;

	case DisconnectReason::ProcessingExc:
	case DisconnectReason::Protocol:
		nByeReason = ByeReason::Ban;
		break;
	}

	DeleteSelf(bIsErr, nByeReason);
}

void Node::Peer::ReleaseTasks()
{
	while (!m_lstTasks.empty())
		ReleaseTask(m_lstTasks.front());
}

void Node::Peer::ReleaseTask(Task& t)
{
	assert(this == t.m_pOwner);
	t.m_pOwner = NULL;

	if (t.m_bPack)
	{
		uint32_t& nCounter = t.m_Key.second ? m_This.m_nTasksPackBody : m_This.m_nTasksPackHdr;
		assert(nCounter);

		nCounter--;
		t.m_bPack = false;
	}

	m_lstTasks.erase(TaskList::s_iterator_to(t));
	m_This.m_lstTasksUnassigned.push_back(t);

	if (t.m_bRelevant)
		m_This.TryAssignTask(t, NULL);
	else
		m_This.DeleteUnassignedTask(t);
}

void Node::Peer::DeleteSelf(bool bIsError, uint8_t nByeReason)
{
	LOG_INFO() << "-Peer " << m_RemoteAddr;

	if (nByeReason && (Flags::Connected & m_Flags))
	{
		proto::Bye msg;
		msg.m_Reason = nByeReason;
		Send(msg);
	}

	ZeroObject(m_Tip); // prevent reassigning the tasks

	ReleaseTasks();
	Unsubscribe();

	if (m_pInfo)
	{
		// detach
		assert(this == m_pInfo->m_pLive);
		m_pInfo->m_pLive = NULL;

		m_This.m_PeerMan.OnActive(*m_pInfo, false);

		if (bIsError)
			m_This.m_PeerMan.OnRemoteError(*m_pInfo, ByeReason::Ban == nByeReason);
	}

	if (m_This.m_pSync && (Flags::SyncPending & m_Flags))
	{
		assert(m_This.m_pSync->m_RequestsPending);
		m_Flags &= ~Flags::SyncPending;
		m_Flags |= Flags::DontSync;
		m_This.m_pSync->m_RequestsPending--;

		m_This.SyncCycle();
	}

	m_This.m_lstPeers.erase(PeerList::s_iterator_to(*this));
	delete this;
}

void Node::Peer::Unsubscribe(Bbs::Subscription& s)
{
	m_This.m_Bbs.m_Subscribed.erase(Bbs::Subscription::BbsSet::s_iterator_to(s.m_Bbs));
	m_Subscriptions.erase(Bbs::Subscription::PeerSet::s_iterator_to(s.m_Peer));
	delete &s;
}

void Node::Peer::Unsubscribe()
{
	while (!m_Subscriptions.empty())
		Unsubscribe(m_Subscriptions.begin()->get_ParentObj());
}

void Node::Peer::TakeTasks()
{
	for (TaskList::iterator it = m_This.m_lstTasksUnassigned.begin(); m_This.m_lstTasksUnassigned.end() != it; )
	{
		Task& t = *(it++);

		if (m_This.ShouldAssignTask(t, *this))
			m_This.AssignTask(t, *this);
	}
}

void Node::Peer::OnMsg(proto::Ping&&)
{
	proto::Pong msg(Zero);
	Send(msg);
}

void Node::Peer::OnMsg(proto::NewTip&& msg)
{
	if (msg.m_Description.m_ChainWork < m_Tip.m_ChainWork)
		ThrowUnexpected();

	m_Tip = msg.m_Description;
	m_setRejected.clear();

	Block::SystemState::ID id;
	m_Tip.get_ID(id);

	LOG_INFO() << "Peer " << m_RemoteAddr << " Tip: " << id;

	if (!m_pInfo)
		return;

	bool bSyncMode = (bool) m_This.m_pSync;
	Processor& p = m_This.m_Processor;

	if (NodeProcessor::IsRemoteTipNeeded(m_Tip, p.m_Cursor.m_Full))
	{
		if (!bSyncMode && (m_Tip.m_Height > p.m_Cursor.m_ID.m_Height + Rules::get().MaxRollbackHeight + Rules::get().MacroblockGranularity * 2))
			LOG_WARNING() << "Height drop is too big, maybe unreachable";

		switch (p.OnState(m_Tip, m_pInfo->m_ID.m_Key))
		{
		case NodeProcessor::DataStatus::Invalid:
			ThrowUnexpected();
			// no break;

		case NodeProcessor::DataStatus::Accepted:
			m_This.m_PeerMan.ModifyRating(*m_pInfo, PeerMan::Rating::RewardHeader, true);

			if (bSyncMode)
				break;

			m_This.RefreshCongestions(); // NOTE! Can call OnPeerInsane()
			return;

		case NodeProcessor::DataStatus::Unreachable:
			LOG_WARNING() << id << " Tip unreachable!";
			break;

		default:
			break; // suppress warning
		}

	}

	if (!bSyncMode)
	{
		TakeTasks();
		return;
	}

	uint8_t nProvenWork = Flags::ProvenWorkReq & m_Flags;
	if (!nProvenWork)
	{
		m_Flags |= Flags::ProvenWorkReq;
		// maybe take it
		Send(proto::GetProofChainWork());
	}

	if (m_This.m_pSync->m_bDetecting)
	{
		if (!nProvenWork/* && (m_This.m_pSync->m_Best <= m_Tip.m_ChainWork)*/)
		{
			// maybe take it
			m_Flags |= Flags::DontSync;
			Send(proto::MacroblockGet());

			LOG_INFO() << " Sending MacroblockGet/query to " << m_RemoteAddr;
		}
	}
	else
		m_This.SyncCycle(*this);
}

void Node::Peer::OnMsg(proto::ProofChainWork&& msg)
{
	Block::SystemState::Full s;
	if (!msg.m_Proof.IsValid(&s))
		ThrowUnexpected();

	if (s.m_ChainWork != m_Tip.m_ChainWork)
		ThrowUnexpected(); // should correspond to the tip, but we're interested only in the asserted work

	LOG_WARNING() << "Peer " << m_RemoteAddr << " Chainwork ok";

	m_Flags |= Flags::ProvenWork;

	if (m_This.m_pSync)
		m_This.SyncCycle();
}

void Node::Peer::OnMsg(proto::Macroblock&& msg)
{
	LOG_INFO() << " Got Macroblock from " << m_RemoteAddr << ". Portion=" << msg.m_Portion.size();

	if (!m_This.m_pSync)
		return;

	if (!(Flags::ProvenWork & m_Flags))
		ThrowUnexpected();

	if (Flags::SyncPending & m_Flags)
	{
		assert(m_This.m_pSync->m_RequestsPending);
		m_Flags &= ~Flags::SyncPending;
		m_This.m_pSync->m_RequestsPending--;

		if (msg.m_ID == m_This.m_pSync->m_Trg)
		{
			LOG_INFO() << "Peer " << m_RemoteAddr << " DL Macroblock portion";
			m_This.SyncCycle(*this, msg.m_Portion);
		}
		else
		{
			LOG_INFO() << "Peer incompatible";

			m_Flags |= Flags::DontSync;
			m_This.SyncCycle();
		}
	}
	else
	{
		m_Flags &= ~Flags::DontSync;

		if (!m_This.m_pSync->m_bDetecting)
			return;

		// still in 1st phase. Check if it's better
		int nCmp = m_This.m_pSync->m_Best.cmp(m_Tip.m_ChainWork);
		if ((nCmp < 0) || (!nCmp && (m_This.m_pSync->m_Trg.m_Height < msg.m_ID.m_Height)))
		{
			LOG_INFO() << "Sync target so far: " << msg.m_ID << ", best Peer " << m_RemoteAddr;

			m_This.m_pSync->m_Trg = msg.m_ID;
			m_This.m_pSync->m_Best = m_Tip.m_ChainWork;

			if (!m_This.m_pSync->m_pTimer)
			{
				m_This.m_pSync->m_pTimer = io::Timer::create(io::Reactor::get_Current());

				Node* pThis = &m_This;
				m_This.m_pSync->m_pTimer->start(m_This.m_Cfg.m_Sync.m_Timeout_ms, false, [pThis]() { pThis->OnSyncTimer(); });
			}

		}

		if (++m_This.m_pSync->m_RequestsPending >= m_This.m_Cfg.m_Sync.m_SrcPeers)
			m_This.OnSyncTimer();
	}
}

void Node::OnSyncTimer()
{
	assert(m_pSync && m_pSync->m_bDetecting);

	if (m_pSync->m_Trg.m_Height)
	{
		m_pSync->m_pTimer = NULL;

		LOG_INFO() << "Sync target final: " << m_pSync->m_Trg;

		Blob blob(m_pSync->m_Trg.m_Hash);
		m_Processor.get_DB().ParamSet(NodeDB::ParamID::SyncTarget, &m_pSync->m_Trg.m_Height, &blob);

		m_pSync->m_bDetecting = false;
		m_pSync->m_RequestsPending = 0;

		SyncCycle();
	}
	else
	{
		m_pSync = NULL;
		LOG_INFO() << "Switching to standard sync";
		RefreshCongestions();
	}
}

void Node::SyncCycle()
{
	assert(m_pSync);
	if (m_pSync->m_bDetecting || m_pSync->m_RequestsPending)
		return;

	for (PeerList::iterator it = m_lstPeers.begin(); m_lstPeers.end() != it; it++)
		if (SyncCycle(*it))
			break;
}

bool Node::SyncCycle(Peer& p)
{
	assert(m_pSync);
	if (m_pSync->m_bDetecting || m_pSync->m_RequestsPending)
		return false;

	assert(!(Peer::Flags::SyncPending & p.m_Flags));
	if (Peer::Flags::DontSync & p.m_Flags)
		return false;

	if (p.m_Tip.m_Height < m_pSync->m_Trg.m_Height/* + Rules::get().MaxRollbackHeight*/)
		return false;

	proto::MacroblockGet msg;
	msg.m_ID = m_pSync->m_Trg;
	msg.m_Data = m_pSync->m_iData;

	assert(m_pSync->m_iData < Block::Body::RW::Type::count);

	Block::Body::RW rw;
	m_Compressor.FmtPath(rw, m_pSync->m_Trg.m_Height, NULL);

	std::string sPath;
	rw.GetPath(sPath, m_pSync->m_iData);

	std::FStream fs;
	if (fs.Open(sPath.c_str(), true))
		msg.m_Offset = fs.get_Remaining();

	p.Send(msg);
	p.m_Flags |= Peer::Flags::SyncPending;
	m_pSync->m_RequestsPending++;

	LOG_INFO() << " Sending MacroblockGet/request to " << p.m_RemoteAddr << ". Idx=" << uint32_t(msg.m_Data) << ", Offset=" << msg.m_Offset;

	return true;
}

void Node::SyncCycle(Peer& p, const ByteBuffer& buf)
{
	assert(m_pSync && !m_pSync->m_bDetecting && !m_pSync->m_RequestsPending);
	assert(m_pSync->m_iData < Block::Body::RW::Type::count);

	if (buf.empty())
	{
		LOG_INFO() << "Sync cycle complete for Idx=" << m_pSync->m_iData;

		if (++m_pSync->m_iData == Block::Body::RW::Type::count)
		{
			Height h = m_pSync->m_Trg.m_Height;
			m_pSync = NULL;

			LOG_INFO() << "Sync DL complete";

			ImportMacroblock(h);
			RefreshCongestions();

			return;
		}
	}
	else
	{
		Block::Body::RW rw;
		m_Compressor.FmtPath(rw, m_pSync->m_Trg.m_Height, NULL);

		std::string sPath;
		rw.GetPath(sPath, m_pSync->m_iData);

		std::FStream fs;
		fs.Open(sPath.c_str(), false, true, true);

		fs.write(&buf.at(0), buf.size());

		LOG_INFO() << "Portion appended";
	}

	SyncCycle(p);
}

Node::Task& Node::Peer::get_FirstTask()
{
	if (m_lstTasks.empty())
		ThrowUnexpected();
	return m_lstTasks.front();
}

void Node::Peer::OnFirstTaskDone()
{
	ReleaseTask(get_FirstTask());
	SetTimerWrtFirstTask();
}

void Node::Peer::OnMsg(proto::DataMissing&&)
{
	Task& t = get_FirstTask();
	m_setRejected.insert(t.m_Key);

	OnFirstTaskDone();
}

void Node::Peer::OnMsg(proto::GetHdr&& msg)
{
	uint64_t rowid = m_This.m_Processor.get_DB().StateFindSafe(msg.m_ID);
	if (rowid)
	{
		proto::Hdr msgHdr;
		m_This.m_Processor.get_DB().get_State(rowid, msgHdr.m_Description);
		Send(msgHdr);
	} else
	{
		proto::DataMissing msgMiss(Zero);
		Send(msgMiss);
	}
}

void Node::Peer::OnMsg(proto::Hdr&& msg)
{
	Task& t = get_FirstTask();

	if (t.m_Key.second || t.m_bPack)
		ThrowUnexpected();

	Block::SystemState::ID id;
	msg.m_Description.get_ID(id);
	if (id != t.m_Key.first)
		ThrowUnexpected();

	assert((Flags::PiRcvd & m_Flags) && m_pInfo);
	m_This.m_PeerMan.ModifyRating(*m_pInfo, PeerMan::Rating::RewardHeader, true);

	NodeProcessor::DataStatus::Enum eStatus = m_This.m_Processor.OnState(msg.m_Description, m_pInfo->m_ID.m_Key);
	OnFirstTaskDone(eStatus);
}

void Node::Peer::OnMsg(proto::GetHdrPack&& msg)
{
	proto::HdrPack msgOut;

	if (msg.m_Count)
	{
		if (msg.m_Count > proto::g_HdrPackMaxSize)
			ThrowUnexpected();

		NodeDB& db = m_This.m_Processor.get_DB();
		uint64_t rowid = db.StateFindSafe(msg.m_Top);
		if (rowid)
		{
			msgOut.m_vElements.reserve(msg.m_Count);

			Block::SystemState::Full s;
			for (uint32_t n = 0; ; )
			{
				db.get_State(rowid, s);
				msgOut.m_vElements.push_back(s);

				if (++n == msg.m_Count)
					break;

				if (!db.get_Prev(rowid))
					break;
			}

			msgOut.m_Prefix = s;
		}
	}

	if (msgOut.m_vElements.empty())
		Send(proto::DataMissing(Zero));
	else
		Send(msgOut);
}

void Node::Peer::OnMsg(proto::HdrPack&& msg)
{
	Task& t = get_FirstTask();

	if (t.m_Key.second || !t.m_bPack)
		ThrowUnexpected();

	if (msg.m_vElements.empty() || (msg.m_vElements.size() > proto::g_HdrPackMaxSize))
		ThrowUnexpected();

	Block::SystemState::Full s;
	((Block::SystemState::Sequence::Prefix&) s) = msg.m_Prefix;
	((Block::SystemState::Sequence::Element&) s) = msg.m_vElements.back();

	uint32_t nAccepted = 0;
	bool bInvalid = false;

	for (size_t i = msg.m_vElements.size(); ; )
	{
		NodeProcessor::DataStatus::Enum eStatus = m_This.m_Processor.OnState(s, m_pInfo->m_ID.m_Key);
		switch (eStatus)
		{
		case NodeProcessor::DataStatus::Invalid:
			bInvalid = true;
			break;

		case NodeProcessor::DataStatus::Accepted:
			nAccepted++;

		default:
			break; // suppress warning
		}

		if (! --i)
			break;

		s.NextPrefix();
		((Block::SystemState::Sequence::Element&) s) = msg.m_vElements[i - 1];
		s.m_PoW.m_Difficulty.Inc(s.m_ChainWork);
	}

	// just to be pedantic
	Block::SystemState::ID id;
	s.get_ID(id);
	if (id != t.m_Key.first)
		bInvalid = true;

	OnFirstTaskDone();

	if (nAccepted)
	{
		assert((Flags::PiRcvd & m_Flags) && m_pInfo);
		m_This.m_PeerMan.ModifyRating(*m_pInfo, PeerMan::Rating::RewardHeader * nAccepted, true);

		m_This.RefreshCongestions(); // may delete us
	} else
		if (bInvalid)
			ThrowUnexpected();

}

void Node::Peer::OnMsg(proto::GetBody&& msg)
{
	uint64_t rowid = m_This.m_Processor.get_DB().StateFindSafe(msg.m_ID);
	if (rowid)
	{
		proto::Body msgBody;
		ByteBuffer bbRollback;
		m_This.m_Processor.get_DB().GetStateBlock(rowid, msgBody.m_Buffer, bbRollback);

		if (!msgBody.m_Buffer.empty())
		{
			Send(msgBody);
			return;
		}

	}

	proto::DataMissing msgMiss(Zero);
	Send(msgMiss);
}

void Node::Peer::OnMsg(proto::Body&& msg)
{
	Task& t = get_FirstTask();

	if (!t.m_Key.second || t.m_bPack)
		ThrowUnexpected();

	assert((Flags::PiRcvd & m_Flags) && m_pInfo);
	m_This.m_PeerMan.ModifyRating(*m_pInfo, PeerMan::Rating::RewardBlock, true);

	const Block::SystemState::ID& id = t.m_Key.first;

	NodeProcessor::DataStatus::Enum eStatus = m_This.m_Processor.OnBlock(id, msg.m_Buffer, m_pInfo->m_ID.m_Key);
	OnFirstTaskDone(eStatus);
}

void Node::Peer::OnFirstTaskDone(NodeProcessor::DataStatus::Enum eStatus)
{
	if (NodeProcessor::DataStatus::Invalid == eStatus)
		ThrowUnexpected();

	get_FirstTask().m_bRelevant = false;
	OnFirstTaskDone();

	if (NodeProcessor::DataStatus::Accepted == eStatus)
		m_This.RefreshCongestions(); // NOTE! Can call OnPeerInsane()
}

void Node::Peer::OnMsg(proto::NewTransaction&& msg)
{
	if (!msg.m_Transaction)
		ThrowUnexpected(); // our deserialization permits NULL Ptrs.
	// However the transaction body must have already been checked for NULLs

	if (msg.m_Fluff)
		m_This.OnTransactionFluff(std::move(msg.m_Transaction), this, NULL);
	else
	{
		proto::Boolean msgOut;
		msgOut.m_Value = m_This.OnTransactionStem(std::move(msg.m_Transaction), this);
		Send(msgOut);
	}
}

bool Node::ValidateTx(Transaction::Context& ctx, const Transaction& tx)
{
	return
		tx.IsValid(ctx) &&
		m_Processor.ValidateTxContext(tx);
}

void Node::LogTx(const Transaction& tx, bool bValid, const Transaction::KeyType& key)
{
	std::ostringstream os;

	os << "Tx " << key;

	for (size_t i = 0; i < tx.m_vInputs.size(); i++)
		os << "\n\tI: " << tx.m_vInputs[i]->m_Commitment;

	for (size_t i = 0; i < tx.m_vOutputs.size(); i++)
	{
		const Output& outp = *tx.m_vOutputs[i];
		os << "\n\tO: " << outp.m_Commitment;

		if (outp.m_Incubation)
			os << ", Incubation +" << outp.m_Incubation;

		if (outp.m_pPublic)
			os << ", Sum=" << outp.m_pPublic->m_Value;

		if (outp.m_pConfidential)
			os << ", Confidential";
	}

	for (size_t i = 0; i < tx.m_vKernelsOutput.size(); i++)
		os << "\n\tK: Fee=" << tx.m_vKernelsOutput[i]->m_Fee;

	os << "\n\tValid: " << bValid;
	LOG_INFO() << os.str();
}

const ECC::uintBig& Node::NextNonce()
{
	ECC::GenerateNonce(m_NonceLast.V, m_Processor.m_Kdf.m_Secret.V, m_NonceLast.V, NULL, 0);
	return m_NonceLast.V;
}

void Node::NextNonce(ECC::Scalar::Native& sk)
{
	sk.GenerateNonce(m_Cfg.m_WalletKey.V, NextNonce(), NULL);
}

uint32_t Node::RandomUInt32(uint32_t threshold)
{
	if (threshold)
	{
		typedef uintBigFor<uint32_t>::Type Type;

		Type thr(threshold), val;
		Type::Threshold thrSel(thr);

		do
		{
			val = NextNonce();
		} while (!thrSel.Accept(val));

		val.Export(threshold);
	}
	return threshold;
}

void CmpTx(const Transaction& tx1, const Transaction& tx2, bool& b1Covers, bool& b2Covers)
{
}

bool Node::OnTransactionStem(Transaction::Ptr&& ptx, const Peer* pPeer)
{
	if (ptx->m_vInputs.empty() || ptx->m_vKernelsOutput.empty())
		return false;

	Transaction::Context ctx;
	bool bTested = false;
	TxPool::Stem::Element* pDup = NULL;

	// find match by kernels
	for (size_t i = 0; i < ptx->m_vKernelsOutput.size(); i++)
	{
		const TxKernel& krn = *ptx->m_vKernelsOutput[i];

		TxPool::Stem::Element::Kernel key;
		krn.get_ID(key.m_hv);

		TxPool::Stem::KrnSet::iterator it = m_Dandelion.m_setKrns.find(key);
		if (m_Dandelion.m_setKrns.end() == it)
			continue;

		TxPool::Stem::Element* pElem = it->m_pThis;
		bool bElemCovers = true, bNewCovers = true;
		pElem->m_pValue->get_Reader().Compare(std::move(ptx->get_Reader()), bElemCovers, bNewCovers);

		if (!bNewCovers)
			return false; // the new tx is reduced, drop it

		if (bElemCovers)
		{
			pDup = pElem; // exact match

			if (pDup->m_bAggregating)
				return true; // it shouldn't have been received, but nevermind, just ignore

			break;
		}

		if (!bTested && !ValidateTx(ctx, *ptx))
			return false;
		bTested = true;

		m_Dandelion.Delete(*pElem);
	}

	if (!pDup)
	{
		if (!bTested && !ValidateTx(ctx, *ptx))
			return false;

		AddDummyInputs(*ptx);

		std::unique_ptr<TxPool::Stem::Element> pGuard(new TxPool::Stem::Element);
		pGuard->m_bAggregating = false;
		pGuard->m_Time.m_Value = 0;
		pGuard->m_Profit.SetFee(ctx);
		pGuard->m_Profit.SetSize(*ptx);
		pGuard->m_pValue.swap(ptx);

		m_Dandelion.InsertKrn(*pGuard);

		pDup = pGuard.release();
	}

	assert(!pDup->m_bAggregating);

	if (pDup->m_pValue->m_vOutputs.size() > m_Cfg.m_Dandelion.m_OutputsMax)
		OnTransactionAggregated(*pDup);
	else
	{
		m_Dandelion.InsertAggr(*pDup);
		PerformAggregation(*pDup);
	}

	return true;
}

void Node::OnTransactionAggregated(TxPool::Stem::Element& x)
{
	// must have at least 1 peer to continue the stem phase
	uint32_t nStemPeers = 0;

	for (PeerList::iterator it = m_lstPeers.begin(); m_lstPeers.end() != it; it++)
		if (it->m_Config.m_SpreadingTransactions)
			nStemPeers++;

	if (nStemPeers)
	{
		auto thr = uintBigFrom(m_Cfg.m_Dandelion.m_FluffProbability);

		if (memcmp(thr.m_pData, NextNonce().m_pData, thr.nBytes) < 0)
		{
			// broadcast to random peer
			assert(nStemPeers);
			nStemPeers = RandomUInt32(nStemPeers);

			for (PeerList::iterator it = m_lstPeers.begin(); ; it++)
				if (it->m_Config.m_SpreadingTransactions && !nStemPeers--)
				{
					it->SendTxGuard(x.m_pValue, false);
					break;
				}

			// set random timer
			uint32_t nTimeout_ms = m_Cfg.m_Dandelion.m_TimeoutMin_ms + RandomUInt32(m_Cfg.m_Dandelion.m_TimeoutMax_ms - m_Cfg.m_Dandelion.m_TimeoutMin_ms);
			m_Dandelion.SetTimer(nTimeout_ms, x);

			return;
		}
	}

	OnTransactionFluff(std::move(x.m_pValue), NULL, &x);
}

void Node::PerformAggregation(TxPool::Stem::Element& x)
{
	assert(x.m_bAggregating);

	// Aggregation policiy: first select those with worse profit, than those with better
	TxPool::Stem::ProfitSet::iterator it = TxPool::Stem::ProfitSet::s_iterator_to(x.m_Profit);
	++it;

	while (x.m_pValue->m_vOutputs.size() <= m_Cfg.m_Dandelion.m_OutputsMax)
	{
		if (m_Dandelion.m_setProfit.end() == it)
			break;

		TxPool::Stem::Element& src = it->get_ParentObj();
		++it;

		m_Dandelion.TryMerge(x, src);
	}

	it = TxPool::Stem::ProfitSet::s_iterator_to(x.m_Profit);
	if (m_Dandelion.m_setProfit.begin() != it)
	{
		--it;
		while (x.m_pValue->m_vOutputs.size() <= m_Cfg.m_Dandelion.m_OutputsMax)
		{
			TxPool::Stem::Element& src = it->get_ParentObj();
			bool bEnd = (m_Dandelion.m_setProfit.begin() == it);

			m_Dandelion.TryMerge(x, src);

			if (bEnd)
				break;
			--it;
		}
	}

	if (x.m_pValue->m_vOutputs.size() >= m_Cfg.m_Dandelion.m_OutputsMin)
	{
		m_Dandelion.DeleteAggr(x);
		OnTransactionAggregated(x);
	} else
		m_Dandelion.SetTimer(m_Cfg.m_Dandelion.m_AggregationTime_ms, x);
}

void Node::AddDummyInputs(Transaction& tx)
{
	NodeDB::Transaction txScope;
	bool bModified = false;

	while (tx.m_vInputs.size() < m_Cfg.m_Dandelion.m_OutputsMax)
	{
		Height h;
		ECC::Scalar sk;
		Blob blob(sk.m_Value);

		uint64_t rowid = m_Processor.get_DB().FindDummy(h, blob);
		if (!rowid || (h > m_Processor.m_Cursor.m_ID.m_Height + 1))
			break;

		if (!bModified)
		{
			bModified = true;
			txScope.Start(m_Processor.get_DB());
		}

		ECC::Mode::Scope scope(ECC::Mode::Fast);

		// bounds
		UtxoTree::Key kMin, kMax;

		UtxoTree::Key::Data d;
		d.m_Commitment = ECC::Context::get().G * sk;
		d.m_Maturity = 0;
		kMin = d;

		d.m_Maturity = m_Processor.m_Cursor.m_ID.m_Height + 1;
		kMax = d;

		// check if it's still unspent
		struct Traveler :public UtxoTree::ITraveler {
			virtual bool OnLeaf(const RadixTree::Leaf& x) override {
				return false;
			}
		} t;

		UtxoTree::Cursor cu;
		t.m_pCu = &cu;
		t.m_pBound[0] = kMin.m_pArr;
		t.m_pBound[1] = kMax.m_pArr;

		if (m_Processor.get_Utxos().Traverse(t))
		{
			// spent
			m_Processor.get_DB().DeleteDummy(rowid);
		}
		else
		{
			// unspent
			Input::Ptr pInp(new Input);
			pInp->m_Commitment = d.m_Commitment;

			tx.m_vInputs.push_back(std::move(pInp));
			tx.m_Offset = ECC::Scalar::Native(tx.m_Offset) + ECC::Scalar::Native(sk);

			/// in the (unlikely) case the tx will be lost - we'll retry spending this UTXO after the following num of blocks
			m_Processor.get_DB().SetDummyHeight(rowid, m_Processor.m_Cursor.m_ID.m_Height + m_Cfg.m_Dandelion.m_DummyLifetimeLo + 1);
		}

	}

	if (bModified)
	{
		txScope.Commit();
		tx.Sort();
	}
}

void Node::AddDummyOutputs(Transaction& tx)
{
	if (!m_Cfg.m_Dandelion.m_DummyLifetimeHi)
		return;

	// add dummy outputs
	NodeDB::Transaction txScope;
	bool bModified = false;

	NodeDB& db = m_Processor.get_DB();

	while (tx.m_vOutputs.size() < m_Cfg.m_Dandelion.m_OutputsMin)
	{
		ECC::Scalar::Native sk;
		NextNonce(sk);

		if (!bModified)
		{
			bModified = true;
			txScope.Start(db);
		}

		Output::Ptr pOutput(new Output);
		pOutput->Create(sk, 0);

		Height h = m_Processor.m_Cursor.m_ID.m_Height + 1 + m_Cfg.m_Dandelion.m_DummyLifetimeLo;
		if (m_Cfg.m_Dandelion.m_DummyLifetimeHi > m_Cfg.m_Dandelion.m_DummyLifetimeLo)
			h += RandomUInt32(m_Cfg.m_Dandelion.m_DummyLifetimeHi - m_Cfg.m_Dandelion.m_DummyLifetimeLo);

		ECC::Scalar sk_(sk); // not so secret
		db.InsertDummy(h, Blob(sk_.m_Value));

		tx.m_vOutputs.push_back(std::move(pOutput));

		sk = -sk;
		tx.m_Offset = ECC::Scalar::Native(tx.m_Offset) + sk;
	}

	if (bModified)
	{
		txScope.Commit();
		tx.Sort();
	}
}

bool Node::OnTransactionFluff(Transaction::Ptr&& ptxArg, const Peer* pPeer, TxPool::Stem::Element* pElem)
{
	Transaction::Ptr ptx;
	ptx.swap(ptxArg);

	Transaction::Context ctx;
	if (pElem)
	{
		ctx.m_Fee.Lo = pElem->m_Profit.m_Fee;
		m_Dandelion.Delete(*pElem);
	}
	else
	{
		for (size_t i = 0; i < ptx->m_vKernelsOutput.size(); i++)
		{
			TxPool::Stem::Element::Kernel key;
			ptx->m_vKernelsOutput[i]->get_ID(key.m_hv);

			TxPool::Stem::KrnSet::iterator it = m_Dandelion.m_setKrns.find(key);
			if (m_Dandelion.m_setKrns.end() != it)
				m_Dandelion.Delete(*it->m_pThis);
		}

	}

	TxPool::Fluff::Element::Tx key;
	ptx->get_Key(key.m_Key);

	TxPool::Fluff::TxSet::iterator it = m_TxPool.m_setTxs.find(key);
	if (m_TxPool.m_setTxs.end() != it)
		return true;

	const Transaction& tx = *ptx;

	m_Wtx.Delete(key.m_Key);

	// new transaction
	bool bValid = pElem ? true: ValidateTx(ctx, tx);
	LogTx(tx, bValid, key.m_Key);

	if (!bValid)
		return false;

	proto::HaveTransaction msgOut;
	msgOut.m_ID = key.m_Key;

	for (PeerList::iterator it2 = m_lstPeers.begin(); m_lstPeers.end() != it2; it2++)
	{
		Peer& peer = *it2;
		if (&peer == pPeer)
			continue;
		if (!peer.m_Config.m_SpreadingTransactions)
			continue;

		peer.Send(msgOut);
	}

	m_TxPool.AddValidTx(std::move(ptx), ctx, key.m_Key);
	m_TxPool.ShrinkUpTo(m_Cfg.m_MaxPoolTransactions);
	m_Miner.SetTimer(m_Cfg.m_Timeout.m_MiningSoftRestart_ms, false);

	return true;
}

void Node::Dandelion::OnTimedOut(Element& x)
{
	if (x.m_bAggregating)
	{
		get_ParentObj().AddDummyOutputs(*x.m_pValue);
		get_ParentObj().OnTransactionAggregated(x);
	} else
		get_ParentObj().OnTransactionFluff(std::move(x.m_pValue), NULL, &x);
}

bool Node::Dandelion::ValidateTxContext(const Transaction& tx)
{
	return get_ParentObj().m_Processor.ValidateTxContext(tx);
}

void Node::Peer::OnMsg(proto::Config&& msg)
{
	if (msg.m_CfgChecksum != Rules::get().Checksum)
		ThrowUnexpected("Incompatible peer cfg!");

	if (!m_Config.m_SpreadingTransactions && msg.m_SpreadingTransactions)
	{
		proto::HaveTransaction msgOut;

		for (TxPool::Fluff::TxSet::iterator it = m_This.m_TxPool.m_setTxs.begin(); m_This.m_TxPool.m_setTxs.end() != it; it++)
		{
			msgOut.m_ID = it->m_Key;
			Send(msgOut);
		}
	}

	if (m_Config.m_SendPeers != msg.m_SendPeers)
	{
		if (msg.m_SendPeers)
		{
			if (!m_pTimerPeers)
				m_pTimerPeers = io::Timer::create(io::Reactor::get_Current());

			m_pTimerPeers->start(m_This.m_Cfg.m_Timeout.m_TopPeersUpd_ms, true, [this]() { OnResendPeers(); });

			OnResendPeers();
		}
		else
			if (m_pTimerPeers)
				m_pTimerPeers->cancel();
	}

	if (!m_Config.m_Bbs && msg.m_Bbs)
	{
		proto::BbsHaveMsg msgOut;

		NodeDB& db = m_This.m_Processor.get_DB();
		NodeDB::WalkerBbs wlk(db);

		for (db.EnumAllBbs(wlk); wlk.MoveNext(); )
		{
			msgOut.m_Key = wlk.m_Data.m_Key;
			Send(msgOut);
		}
	}

	m_Config = msg;
}

void Node::Peer::OnMsg(proto::HaveTransaction&& msg)
{
	TxPool::Fluff::Element::Tx key;
	key.m_Key = msg.m_ID;

	TxPool::Fluff::TxSet::iterator it = m_This.m_TxPool.m_setTxs.find(key);
	if (m_This.m_TxPool.m_setTxs.end() != it)
		return; // already have it

	if (!m_This.m_Wtx.Add(key.m_Key))
		return; // already waiting for it

	proto::GetTransaction msgOut;
	msgOut.m_ID = msg.m_ID;
	Send(msgOut);
}

void Node::Peer::OnMsg(proto::GetTransaction&& msg)
{
	TxPool::Fluff::Element::Tx key;
	key.m_Key = msg.m_ID;

	TxPool::Fluff::TxSet::iterator it = m_This.m_TxPool.m_setTxs.find(key);
	if (m_This.m_TxPool.m_setTxs.end() == it)
		return; // don't have it

	SendTxGuard(it->get_ParentObj().m_pValue, true);
}

void Node::Peer::SendTxGuard(Transaction::Ptr& ptx, bool bFluff)
{
	// temporarily move the transaction to the Msg object, but make sure it'll be restored back, even in case of the exception.
	struct Guard
	{
		proto::NewTransaction m_Msg;
		Transaction::Ptr* m_ppVal;

		void Swap() { m_ppVal->swap(m_Msg.m_Transaction); }

		~Guard() { Swap(); }
	};

	Guard g;
	g.m_ppVal = &ptx;
	g.Swap();
	g.m_Msg.m_Fluff = bFluff;

	Send(g.m_Msg);
}

void Node::Peer::OnMsg(proto::GetMined&& msg)
{
	proto::Mined msgOut;

	if ((Flags::Owner & m_Flags) || !m_This.m_Cfg.m_RestrictMinedReportToOwner)
	{
		NodeDB& db = m_This.m_Processor.get_DB();
		NodeDB::WalkerMined wlk(db);
		for (db.EnumMined(wlk, msg.m_HeightMin); wlk.MoveNext(); )
		{
			msgOut.m_Entries.resize(msgOut.m_Entries.size() + 1);
			proto::PerMined& x = msgOut.m_Entries.back();

			x.m_Fees = wlk.m_Amount;
			x.m_Active = 0 != (db.GetStateFlags(wlk.m_Sid.m_Row) & NodeDB::StateFlags::Active);

			db.get_StateID(wlk.m_Sid, x.m_ID);

			if (msgOut.m_Entries.size() == proto::PerMined::s_EntriesMax)
				break;
		}
	} else
		LOG_WARNING() << "Peer " << m_RemoteAddr << " Unauthorized Mining report request. Returned empty result.";

	Send(msgOut);
}

void Node::Peer::OnMsg(proto::GetProofState&& msg)
{
	if (msg.m_Height < Rules::HeightGenesis)
		ThrowUnexpected();

	proto::ProofState msgOut;

	Processor& p = m_This.m_Processor;
	const NodeDB::StateID& sid = p.m_Cursor.m_Sid;
	if (sid.m_Row && (msg.m_Height < sid.m_Height))
	{
		Merkle::ProofBuilderHard bld;
		p.get_DB().get_Proof(bld, sid, msg.m_Height);
		msgOut.m_Proof.swap(bld.m_Proof);

		msgOut.m_Proof.resize(msgOut.m_Proof.size() + 1);
		p.get_CurrentLive(msgOut.m_Proof.back());
	}

	Send(msgOut);
}

void Node::Peer::OnMsg(proto::GetProofKernel&& msg)
{
	proto::ProofKernel msgOut;

	RadixHashOnlyTree& t = m_This.m_Processor.get_Kernels();

	RadixHashOnlyTree::Cursor cu;
	bool bCreate = false;
	if (t.Find(cu, msg.m_ID, bCreate))
	{
		t.get_Proof(msgOut.m_Proof, cu);
		msgOut.m_Proof.reserve(msgOut.m_Proof.size() + 2);

		msgOut.m_Proof.resize(msgOut.m_Proof.size() + 1);
		msgOut.m_Proof.back().first = false;
		m_This.m_Processor.get_Utxos().get_Hash(msgOut.m_Proof.back().second);

		msgOut.m_Proof.resize(msgOut.m_Proof.size() + 1);
		msgOut.m_Proof.back().first = false;
		msgOut.m_Proof.back().second = m_This.m_Processor.m_Cursor.m_History;
	}

	Send(msgOut);
}

void Node::Peer::OnMsg(proto::GetProofUtxo&& msg)
{
	struct Traveler :public UtxoTree::ITraveler
	{
		proto::ProofUtxo m_Msg;
		UtxoTree* m_pTree;
		Merkle::Hash m_hvHistory;
		Merkle::Hash m_hvKernels;

		virtual bool OnLeaf(const RadixTree::Leaf& x) override {

			const UtxoTree::MyLeaf& v = (UtxoTree::MyLeaf&) x;
			UtxoTree::Key::Data d;
			d = v.m_Key;

			m_Msg.m_Proofs.resize(m_Msg.m_Proofs.size() + 1);
			Input::Proof& ret = m_Msg.m_Proofs.back();

			ret.m_State.m_Count = v.m_Value.m_Count;
			ret.m_State.m_Maturity = d.m_Maturity;
			m_pTree->get_Proof(ret.m_Proof, *m_pCu);

			ret.m_Proof.reserve(ret.m_Proof.size() + 2);

			ret.m_Proof.resize(ret.m_Proof.size() + 1);
			ret.m_Proof.back().first = true;
			ret.m_Proof.back().second = m_hvKernels;

			ret.m_Proof.resize(ret.m_Proof.size() + 1);
			ret.m_Proof.back().first = false;
			ret.m_Proof.back().second = m_hvHistory;

			return m_Msg.m_Proofs.size() < Input::Proof::s_EntriesMax;
		}
	} t;

	t.m_pTree = &m_This.m_Processor.get_Utxos();
	m_This.m_Processor.get_Kernels().get_Hash(t.m_hvKernels);
	t.m_hvHistory = m_This.m_Processor.m_Cursor.m_History;

	UtxoTree::Cursor cu;
	t.m_pCu = &cu;

	// bounds
	UtxoTree::Key kMin, kMax;

	UtxoTree::Key::Data d;
	d.m_Commitment = msg.m_Utxo.m_Commitment;
	d.m_Maturity = msg.m_MaturityMin;
	kMin = d;
	d.m_Maturity = Height(-1);
	kMax = d;

	t.m_pBound[0] = kMin.m_pArr;
	t.m_pBound[1] = kMax.m_pArr;

	t.m_pTree->Traverse(t);

	Send(t.m_Msg);
}

bool Node::Processor::BuildCwp()
{
	if (!m_Cwp.IsEmpty())
		return true; // already built

	if (m_Cursor.m_Full.m_Height < Rules::HeightGenesis)
		return false;

	struct Source
		:public Block::ChainWorkProof::ISource
	{
		Processor& m_Proc;
		Source(Processor& proc) :m_Proc(proc) {}

		virtual void get_StateAt(Block::SystemState::Full& s, const Difficulty::Raw& d) override
		{
			uint64_t rowid = m_Proc.get_DB().FindStateWorkGreater(d);
			m_Proc.get_DB().get_State(rowid, s);
		}

		virtual void get_Proof(Merkle::IProofBuilder& bld, Height h) override
		{
			const NodeDB::StateID& sid = m_Proc.m_Cursor.m_Sid;
			m_Proc.get_DB().get_Proof(bld, sid, h);
		}
	};

	Source src(*this);

	m_Cwp.Create(src, m_Cursor.m_Full);
	get_CurrentLive(m_Cwp.m_hvRootLive);

	return true;
}

void Node::Peer::OnMsg(proto::GetProofChainWork&& msg)
{
	proto::ProofChainWork msgOut;

	Processor& p = m_This.m_Processor;
	if (p.BuildCwp())
	{
		msgOut.m_Proof.m_LowerBound = msg.m_LowerBound;
		verify(msgOut.m_Proof.Crop(p.m_Cwp));
	}

	Send(msgOut);
}

void Node::Peer::OnMsg(proto::PeerInfoSelf&& msg)
{
	m_Port = msg.m_Port;
}

void Node::Peer::OnMsg(proto::PeerInfo&& msg)
{
	if (msg.m_ID != m_This.m_MyPublicID)
		m_This.m_PeerMan.OnPeer(msg.m_ID, msg.m_LastAddr, false);
}

void Node::Peer::OnMsg(proto::GetTime&& msg)
{
	proto::Time msgOut;
	msgOut.m_Value = getTimestamp();
	Send(msgOut);
}

void Node::Peer::OnMsg(proto::GetExternalAddr&& msg)
{
	proto::ExternalAddr msgOut;
	msgOut.m_Value = m_RemoteAddr.ip();
	Send(msgOut);
}

void Node::Peer::OnMsg(proto::BbsMsg&& msg)
{
	Timestamp t = getTimestamp();
	Timestamp t0 = t - m_This.m_Cfg.m_Timeout.m_BbsMessageTimeout_s;
	Timestamp t1 = t + m_This.m_Cfg.m_Timeout.m_BbsMessageMaxAhead_s;

	if ((msg.m_TimePosted <= t0) || (msg.m_TimePosted > t1))
		return;

	NodeDB& db = m_This.m_Processor.get_DB();
	NodeDB::WalkerBbs wlk(db);

	wlk.m_Data.m_Channel = msg.m_Channel;
	wlk.m_Data.m_TimePosted = msg.m_TimePosted;
	wlk.m_Data.m_Message = Blob(msg.m_Message);

	Bbs::CalcMsgKey(wlk.m_Data);

	if (db.BbsFind(wlk))
		return; // already have it

	m_This.m_Bbs.MaybeCleanup();

	db.BbsIns(wlk.m_Data);
	m_This.m_Bbs.m_W.Delete(wlk.m_Data.m_Key);

	// 1. Send to other BBS-es

	proto::BbsHaveMsg msgOut;
	msgOut.m_Key = wlk.m_Data.m_Key;

	for (PeerList::iterator it = m_This.m_lstPeers.begin(); m_This.m_lstPeers.end() != it; it++)
	{
		Peer& peer = *it;
		if (this == &peer)
			continue;
		if (!peer.m_Config.m_Bbs)
			continue;

		peer.Send(msgOut);
	}

	// 2. Send to subscribed
	typedef Bbs::Subscription::BbsSet::iterator It;

	Bbs::Subscription::InBbs key;
	key.m_Channel = msg.m_Channel;

	for (std::pair<It, It> range = m_This.m_Bbs.m_Subscribed.equal_range(key); range.first != range.second; range.first++)
	{
		Bbs::Subscription& s = range.first->get_ParentObj();

		if (this == s.m_pPeer)
			continue;

		s.m_pPeer->SendBbsMsg(wlk.m_Data);
	}
}

void Node::Peer::OnMsg(proto::BbsHaveMsg&& msg)
{
	NodeDB& db = m_This.m_Processor.get_DB();
	NodeDB::WalkerBbs wlk(db);

	wlk.m_Data.m_Key = msg.m_Key;
	if (db.BbsFind(wlk))
		return; // already have it

	if (!m_This.m_Bbs.m_W.Add(msg.m_Key))
		return; // already waiting for it

	proto::BbsGetMsg msgOut;
	msgOut.m_Key = msg.m_Key;
	Send(msgOut);
}

void Node::Peer::OnMsg(proto::BbsGetMsg&& msg)
{
	NodeDB& db = m_This.m_Processor.get_DB();
	NodeDB::WalkerBbs wlk(db);

	wlk.m_Data.m_Key = msg.m_Key;
	if (!db.BbsFind(wlk))
		return; // don't have it

	SendBbsMsg(wlk.m_Data);
}

void Node::Peer::SendBbsMsg(const NodeDB::WalkerBbs::Data& d)
{
	proto::BbsMsg msgOut;
	msgOut.m_Channel = d.m_Channel;
	msgOut.m_TimePosted = d.m_TimePosted;
	d.m_Message.Export(msgOut.m_Message); // TODO: avoid buf allocation

	Send(msgOut);
}

void Node::Peer::OnMsg(proto::BbsSubscribe&& msg)
{
	Bbs::Subscription::InPeer key;
	key.m_Channel = msg.m_Channel;

	Bbs::Subscription::PeerSet::iterator it = m_Subscriptions.find(key);
	if ((m_Subscriptions.end() == it) != msg.m_On)
		return;

	if (msg.m_On)
	{
		Bbs::Subscription* pS = new Bbs::Subscription;
		pS->m_pPeer = this;

		pS->m_Bbs.m_Channel = msg.m_Channel;
		pS->m_Peer.m_Channel = msg.m_Channel;
		m_This.m_Bbs.m_Subscribed.insert(pS->m_Bbs);
		m_Subscriptions.insert(pS->m_Peer);

		NodeDB& db = m_This.m_Processor.get_DB();
		NodeDB::WalkerBbs wlk(db);

		wlk.m_Data.m_Channel = msg.m_Channel;
		wlk.m_Data.m_TimePosted = msg.m_TimeFrom;

		for (db.EnumBbs(wlk); wlk.MoveNext(); )
			SendBbsMsg(wlk.m_Data);
	}
	else
		Unsubscribe(it->get_ParentObj());
}

void Node::Peer::OnMsg(proto::BbsPickChannel&& msg)
{
	proto::BbsPickChannelRes msgOut;
	msgOut.m_Channel = m_This.m_Bbs.m_RecommendedChannel;
	Send(msgOut);
}

void Node::Peer::OnMsg(proto::MacroblockGet&& msg)
{
	if (msg.m_Data >= Block::BodyBase::RW::Type::count)
		ThrowUnexpected();

	proto::Macroblock msgOut;

	if (m_This.m_Cfg.m_HistoryCompression.m_UploadPortion)
	{
		Processor& p = m_This.m_Processor;
		NodeDB::WalkerState ws(p.get_DB());
		for (p.get_DB().EnumMacroblocks(ws); ws.MoveNext(); )
		{
			Block::SystemState::ID id;
			p.get_DB().get_StateID(ws.m_Sid, id);

			if (msg.m_ID.m_Height)
			{
				if (msg.m_ID.m_Height < ws.m_Sid.m_Height)
					continue;

				if (id != msg.m_ID)
					break;

				// don't care if exc
				Block::Body::RW rw;
				m_This.m_Compressor.FmtPath(rw, ws.m_Sid.m_Height, NULL);

				std::string sPath;
				rw.GetPath(sPath, msg.m_Data);

				std::FStream fs;
				if (fs.Open(sPath.c_str(), true) && (fs.get_Remaining() > msg.m_Offset))
				{
					uint64_t nDelta = fs.get_Remaining() - msg.m_Offset;

					uint32_t nPortion = m_This.m_Cfg.m_HistoryCompression.m_UploadPortion;
					if (nPortion > nDelta)
						nPortion = (uint32_t)nDelta;

					fs.Seek(msg.m_Offset);

					msgOut.m_Portion.resize(nPortion);
					fs.read(&msgOut.m_Portion.at(0), nPortion);
				}
			}

			msgOut.m_ID = id;

			break;
		}
	}

	Send(msgOut);
}

void Node::Server::OnAccepted(io::TcpStream::Ptr&& newStream, int errorCode)
{
	if (newStream)
	{
		LOG_DEBUG() << "New peer connected: " << newStream->address();
		Peer* p = get_ParentObj().AllocPeer(newStream->peer_address());
		p->Accept(std::move(newStream));
		p->SecureConnect();
	}
}

void Node::Miner::Initialize()
{
	const Config& cfg = get_ParentObj().m_Cfg;
	if (!cfg.m_MiningThreads)
		return;

	m_pEvtMined = io::AsyncEvent::create(io::Reactor::get_Current(), [this]() { OnMined(); });

	m_vThreads.resize(cfg.m_MiningThreads);
	for (uint32_t i = 0; i < cfg.m_MiningThreads; i++)
	{
		PerThread& pt = m_vThreads[i];
		pt.m_pReactor = io::Reactor::create();
		pt.m_pEvt = io::AsyncEvent::create(*pt.m_pReactor, [this, i]() { OnRefresh(i); });
		pt.m_Thread = std::thread(&io::Reactor::run, pt.m_pReactor);
	}

	SetTimer(0, true); // async start mining, since this method may be followed by ImportMacroblock.
}

void Node::Miner::OnRefresh(uint32_t iIdx)
{
	while (true)
	{
		Task::Ptr pTask;
		Block::SystemState::Full s;

		{
			std::scoped_lock<std::mutex> scope(m_Mutex);
			if (!m_pTask || *m_pTask->m_pStop)
				break;

			pTask = m_pTask;
			s = pTask->m_Hdr; // local copy
		}

		ECC::Hash::Value hv; // pick pseudo-random initial nonce for mining.
		ECC::Hash::Processor()
			<< get_ParentObj().m_MyPublicID
			<< iIdx
			<< s.m_Height
			>> hv;

		static_assert(s.m_PoW.m_Nonce.nBytes <= hv.nBytes);
		s.m_PoW.m_Nonce = hv;

		LOG_INFO() << "Mining nonce = " << s.m_PoW.m_Nonce;

		Block::PoW::Cancel fnCancel = [this, pTask](bool bRetrying)
		{
			if (*pTask->m_pStop)
				return true;

			if (bRetrying)
			{
				std::scoped_lock<std::mutex> scope(m_Mutex);
				if (pTask != m_pTask)
					return true; // soft restart triggered
			}

			return false;
		};

		if (Rules::get().FakePoW)
		{
			uint32_t timeout_ms = get_ParentObj().m_Cfg.m_TestMode.m_FakePowSolveTime_ms;

			bool bSolved = false;

			for (uint32_t t0_ms = GetTime_ms(); ; )
			{
				if (fnCancel(false))
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(50));

				uint32_t dt_ms = GetTime_ms() - t0_ms;

				if (dt_ms >= timeout_ms)
				{
					bSolved = true;
					break;
				}
			}

			if (!bSolved)
				continue;

			ZeroObject(s.m_PoW.m_Indices); // keep the difficulty intact
		}
		else
		{
			if (!s.GeneratePoW(fnCancel))
				continue;
		}

		std::scoped_lock<std::mutex> scope(m_Mutex);

		if (*pTask->m_pStop)
			continue; // either aborted, or other thread was faster

		pTask->m_Hdr = s; // save the result
		*pTask->m_pStop = true;
		m_pTask = pTask; // In case there was a soft restart we restore the one that we mined.

		m_pEvtMined->post();
		break;
	}
}

void Node::Miner::HardAbortSafe()
{
	std::scoped_lock<std::mutex> scope(m_Mutex);

	if (m_pTask)
	{
		*m_pTask->m_pStop = true;
		m_pTask = NULL;
	}
}

void Node::Miner::SetTimer(uint32_t timeout_ms, bool bHard)
{
	if (!m_pTimer)
		m_pTimer = io::Timer::create(io::Reactor::get_Current());
	else
		if (m_bTimerPending && !bHard)
			return;

	m_pTimer->start(timeout_ms, false, [this]() { OnTimer(); });
	m_bTimerPending = true;
}

void Node::Miner::OnTimer()
{
	m_bTimerPending = false;
	Restart();
}

bool Node::Miner::Restart()
{
	if (m_vThreads.empty())
		return false; //  n/a

	Block::Body* pTreasury = NULL;

	if (get_ParentObj().m_Processor.m_Extra.m_SubsidyOpen)
	{
		Height dh = get_ParentObj().m_Processor.m_Cursor.m_Sid.m_Height + 1 - Rules::HeightGenesis;
		std::vector<Block::Body>& vTreasury = get_ParentObj().m_Cfg.m_vTreasury;
		if (dh >= vTreasury.size())
			return false;

		pTreasury = &vTreasury[dh];
		pTreasury->m_SubsidyClosing = (dh + 1 == vTreasury.size());
	}

	Task::Ptr pTask(std::make_shared<Task>());

	bool bRes = pTreasury ?
		get_ParentObj().m_Processor.GenerateNewBlock(get_ParentObj().m_TxPool, pTask->m_Hdr, pTask->m_Body, pTask->m_Fees, *pTreasury) :
		get_ParentObj().m_Processor.GenerateNewBlock(get_ParentObj().m_TxPool, pTask->m_Hdr, pTask->m_Body, pTask->m_Fees);

	if (!bRes)
	{
		LOG_WARNING() << "Block generation failed, can't mine!";
		return false;
	}

	LOG_INFO() << "Block generated: Height=" << pTask->m_Hdr.m_Height << ", Fee=" << pTask->m_Fees << ", Difficulty=" << pTask->m_Hdr.m_PoW.m_Difficulty << ", Size=" << pTask->m_Body.size();

	// let's mine it.
	std::scoped_lock<std::mutex> scope(m_Mutex);

	if (m_pTask)
	{
		if (*m_pTask->m_pStop)
			return true; // block already mined, probably notification to this thread on its way. Ignore the newly-constructed block
		pTask->m_pStop = m_pTask->m_pStop; // use the same soft-restart indicator
	}
	else
	{
		pTask->m_pStop.reset(new volatile bool);
		*pTask->m_pStop = false;
	}

	m_pTask = pTask;

	for (size_t i = 0; i < m_vThreads.size(); i++)
		m_vThreads[i].m_pEvt->post();

	return true;
}

void Node::Miner::OnMined()
{
	Task::Ptr pTask;
	{
		std::scoped_lock<std::mutex> scope(m_Mutex);
		if (!(m_pTask && *m_pTask->m_pStop))
			return; //?!
		pTask.swap(m_pTask);
	}

	Block::SystemState::ID id;
	pTask->m_Hdr.get_ID(id);

	LOG_INFO() << "New block mined: " << id;

	NodeProcessor::DataStatus::Enum eStatus = get_ParentObj().m_Processor.OnState(pTask->m_Hdr, get_ParentObj().m_MyPublicID);
	switch (eStatus)
	{
	default:
	case NodeProcessor::DataStatus::Invalid:
		// Some bug?
		LOG_WARNING() << "Mined block rejected as invalid!";
		return;

	case NodeProcessor::DataStatus::Rejected:
		// Someone else mined exactly the same block!
		LOG_WARNING() << "Mined block duplicated";
		return;

	case NodeProcessor::DataStatus::Accepted:
		break; // ok
	}
	assert(NodeProcessor::DataStatus::Accepted == eStatus);

	NodeDB::StateID sid;
	sid.m_Row = get_ParentObj().m_Processor.get_DB().StateFindSafe(id);
	assert(sid.m_Row);
	sid.m_Height = id.m_Height;

	get_ParentObj().m_Processor.get_DB().SetMined(sid, pTask->m_Fees); // ding!

	eStatus = get_ParentObj().m_Processor.OnBlock(id, pTask->m_Body, get_ParentObj().m_MyPublicID); // will likely trigger OnNewState(), and spread this block to the network
	assert(NodeProcessor::DataStatus::Accepted == eStatus);
}

struct Node::Beacon::OutCtx
{
	int m_Refs;
	OutCtx() :m_Refs(0) {}

	struct UvRequest
		:public uv_udp_send_t
	{
		IMPLEMENT_GET_PARENT_OBJ(OutCtx, m_Request)
	} m_Request;

	uv_buf_t m_BufDescr;

#pragma pack (push, 1)
	struct Message
	{
		Merkle::Hash m_CfgChecksum;
		PeerID m_NodeID;
		uint16_t m_Port; // in network byte order
	};
#pragma pack (pop)

	Message m_Message; // the message broadcasted

	void Release()
	{
		assert(m_Refs > 0);
		if (!--m_Refs)
			delete this;
	}

	static void OnDone(uv_udp_send_t* req, int status);
};

Node::Beacon::Beacon()
	:m_pUdp(NULL)
	,m_pOut(NULL)
{
}

Node::Beacon::~Beacon()
{
	if (m_pUdp)
		uv_close((uv_handle_t*) m_pUdp, OnClosed);

	if (m_pOut)
		m_pOut->Release();
}

uint16_t Node::Beacon::get_Port()
{
	uint16_t nPort = get_ParentObj().m_Cfg.m_BeaconPort;
	return nPort ? nPort : get_ParentObj().m_Cfg.m_Listen.port();
}

void Node::Beacon::Start()
{
	assert(!m_pUdp);
	m_pUdp = new uv_udp_t;

	uv_udp_init(&io::Reactor::get_Current().get_UvLoop(), m_pUdp);
	m_pUdp->data = this;

	m_BufRcv.resize(sizeof(OutCtx::Message));

	io::Address addr;
	addr.port(get_Port());

	sockaddr_in sa;
	addr.fill_sockaddr_in(sa);

	if (uv_udp_bind(m_pUdp, (sockaddr*)&sa, UV_UDP_REUSEADDR)) // should allow multiple nodes on the same machine (for testing)
		std::ThrowIoError();

	if (uv_udp_recv_start(m_pUdp, AllocBuf, OnRcv))
		std::ThrowIoError();

	if (uv_udp_set_broadcast(m_pUdp, 1))
		std::ThrowIoError();

	m_pTimer = io::Timer::create(io::Reactor::get_Current());
	m_pTimer->start(get_ParentObj().m_Cfg.m_BeaconPeriod_ms, true, [this]() { OnTimer(); }); // periodic timer
	OnTimer();
}

void Node::Beacon::OnTimer()
{
	if (!m_pOut)
	{
		m_pOut = new OutCtx;
		m_pOut->m_Refs = 1;

		m_pOut->m_Message.m_CfgChecksum = Rules::get().Checksum;
		m_pOut->m_Message.m_NodeID = get_ParentObj().m_MyPublicID;
		m_pOut->m_Message.m_Port = htons(get_ParentObj().m_Cfg.m_Listen.port());

		m_pOut->m_BufDescr.base = (char*) &m_pOut->m_Message;
		m_pOut->m_BufDescr.len = sizeof(m_pOut->m_Message);
	}
	else
		if (m_pOut->m_Refs > 1)
			return; // send still pending

	io::Address addr;
	addr.port(get_Port());
	addr.ip(INADDR_BROADCAST);

	sockaddr_in sa;
	addr.fill_sockaddr_in(sa);

	m_pOut->m_Refs++;

	int nErr = uv_udp_send(&m_pOut->m_Request, m_pUdp, &m_pOut->m_BufDescr, 1, (sockaddr*) &sa, OutCtx::OnDone);
	if (nErr)
		m_pOut->Release();
}

void Node::Beacon::OutCtx::OnDone(uv_udp_send_t* req, int /* status */)
{
	UvRequest* pVal = (UvRequest*)req;
	assert(pVal);

	pVal->get_ParentObj().Release();
}

void Node::Beacon::OnRcv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* pSa, unsigned flags)
{
	OutCtx::Message msg;
	if (sizeof(msg) != nread)
		return;

	memcpy(&msg, buf->base, sizeof(msg)); // copy it to prevent (potential) datatype misallignment and etc.

	if (msg.m_CfgChecksum != Rules::get().Checksum)
		return;

	Beacon* pThis = (Beacon*)handle->data;

	if (pThis->get_ParentObj().m_MyPublicID == msg.m_NodeID)
		return;

	io::Address addr(*(sockaddr_in*)pSa);
	addr.port(ntohs(msg.m_Port));

	pThis->get_ParentObj().m_PeerMan.OnPeer(msg.m_NodeID, addr, true);
}

void Node::Beacon::AllocBuf(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
	Beacon* pThis = (Beacon*)handle->data;
	assert(pThis);

	buf->base = (char*) &pThis->m_BufRcv.at(0);
	buf->len = sizeof(OutCtx::Message);
}

void Node::Beacon::OnClosed(uv_handle_t* p)
{
	assert(p);
	delete (uv_udp_t*) p;
}

void Node::PeerMan::Initialize()
{
	const Config& cfg = get_ParentObj().m_Cfg;

	for (uint32_t i = 0; i < cfg.m_Connect.size(); i++)
	{
		PeerID id0(Zero);
		OnPeer(id0, cfg.m_Connect[i], true);
	}

	// peers
	m_pTimerUpd = io::Timer::create(io::Reactor::get_Current());
	m_pTimerUpd->start(cfg.m_Timeout.m_PeersUpdate_ms, true, [this]() { Update(); });

	m_pTimerFlush = io::Timer::create(io::Reactor::get_Current());
	m_pTimerFlush->start(cfg.m_Timeout.m_PeersDbFlush_ms, true, [this]() { OnFlush(); });

	{
		NodeDB::WalkerPeer wlk(get_ParentObj().m_Processor.get_DB());
		for (get_ParentObj().m_Processor.get_DB().EnumPeers(wlk); wlk.MoveNext(); )
		{
			if (wlk.m_Data.m_ID == get_ParentObj().m_MyPublicID)
				continue; // could be left from previous run?

			PeerMan::PeerInfo* pPi = OnPeer(wlk.m_Data.m_ID, io::Address::from_u64(wlk.m_Data.m_Address), false);
			if (!pPi)
				continue;

			// set rating (akward, TODO - fix this)
			uint32_t r = wlk.m_Data.m_Rating;
			if (!r)
				Ban(*pPi);
			else
				if (r > pPi->m_RawRating.m_Value)
					ModifyRating(*pPi, r - pPi->m_RawRating.m_Value, true);
				else
					ModifyRating(*pPi, pPi->m_RawRating.m_Value - r, false);

			pPi->m_LastSeen = wlk.m_Data.m_LastSeen;
		}
	}
}

void Node::PeerMan::OnFlush()
{
	NodeDB& db = get_ParentObj().m_Processor.get_DB();
	NodeDB::Transaction t(db);

	db.PeersDel();

	const PeerMan::RawRatingSet& rs = get_Ratings();

	for (PeerMan::RawRatingSet::const_iterator it = rs.begin(); rs.end() != it; it++)
	{
		const PeerMan::PeerInfo& pi = it->get_ParentObj();

		NodeDB::WalkerPeer::Data d;
		d.m_ID = pi.m_ID.m_Key;
		d.m_Rating = pi.m_RawRating.m_Value;
		d.m_Address = pi.m_Addr.m_Value.u64();
		d.m_LastSeen = pi.m_LastSeen;

		db.PeerIns(d);
	}

	t.Commit();
}

void Node::PeerMan::ActivatePeer(PeerInfo& pi)
{
	PeerInfoPlus& pip = (PeerInfoPlus&)pi;
	if (pip.m_pLive)
		return; //?

	Peer* p = get_ParentObj().AllocPeer(pip.m_Addr.m_Value);
	p->m_pInfo = &pip;
	pip.m_pLive = p;

	p->Connect(pip.m_Addr.m_Value);
	p->m_Port = pip.m_Addr.m_Value.port();
}

void Node::PeerMan::DeactivatePeer(PeerInfo& pi)
{
	PeerInfoPlus& pip = (PeerInfoPlus&)pi;
	if (!pip.m_pLive)
		return; //?

	pip.m_pLive->DeleteSelf(false, proto::NodeConnection::ByeReason::Other);
}

proto::PeerManager::PeerInfo* Node::PeerMan::AllocPeer()
{
	PeerInfoPlus* p = new PeerInfoPlus;
	p->m_pLive = NULL;
	return p;
}

void Node::PeerMan::DeletePeer(PeerInfo& pi)
{
	delete (PeerInfoPlus*)&pi;
}

} // namespace beam