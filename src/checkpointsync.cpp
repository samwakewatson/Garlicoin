// Copyright (c) 2013-2019 Feathercoin developers
// Copyright (c) 2011-2013 PPCoin developers
// Copyright (c) 2013 Primecoin developers
// Distributed under conditional MIT/X11 software license,
// see the accompanying file COPYING
//
// The synchronized checkpoint system is first developed by Sunny King for
// ppcoin network in 2012, giving cryptocurrency developers a tool to gain
// additional network protection against 51% attack.
//
// Primecoin also adopts this security mechanism, and the enforcement of
// checkpoints is explicitly granted by user, thus granting only temporary
// consensual central control to developer at the threats of 51% attack.
//
// Concepts
//
// In the network there can be a privileged node known as 'checkpoint master'.
// This node can send out checkpoint messages signed by the checkpoint master
// key. Each checkpoint is a block hash, representing a block on the blockchain
// that the network should reach consensus on.
//
// Besides verifying signatures of checkpoint messages, each node also verifies
// the consistency of the checkpoints. If a conflicting checkpoint is received,
// it means either the checkpoint master key is compromised, or there is an
// operator mistake. In this situation the node would discard the conflicting
// checkpoint message and display a warning message. This precaution controls
// the damage to network caused by operator mistake or compromised key.
//
// Operations
//
// Any node can be turned into checkpoint master by setting the 'checkpointkey'
// configuration parameter with the private key of the checkpoint master key.
// Operator should exercise caution such that at any moment there is at most
// one node operating as checkpoint master. When switching master node, the
// recommended procedure is to shutdown the master node and restart as
// regular node, note down the current checkpoint by 'getcheckpoint', then
// compare to the checkpoint at the new node to be upgraded to master node.
// When the checkpoint on both nodes match then it is safe to switch the new
// node to checkpoint master.
//
// The configuration parameter 'checkpointdepth' specifies how many blocks
// should the checkpoints lag behind the latest block in auto checkpoint mode.
// A depth of 5 is the minimum auto checkpoint policy and offers the greatest
// protection against 51% attack.
//

#include <checkpointsync.h>

#include <chainparams.h>
#include <base58.h>
#include <util.h>
#include <netmessagemaker.h>
#include <txdb.h>
#include <validation.h>

// Synchronized checkpoint (centrally broadcasted)
std::string CSyncCheckpoint::strMasterPrivKey;
uint256 hashSyncCheckpoint;
static uint256 hashPendingCheckpoint;
CSyncCheckpoint checkpointMessage;
static CSyncCheckpoint checkpointMessagePending;


// Only descendant of current sync-checkpoint is allowed
bool ValidateSyncCheckpoint(uint256 hashCheckpoint)
{
    CBlockIndex* pindexSyncCheckpoint;
    CBlockIndex* pindexCheckpointRecv;

    {
        LOCK(cs_main);

        if (!::BlockIndex().count(hashSyncCheckpoint))
            return error("%s: block index missing for current sync-checkpoint %s", __func__, hashSyncCheckpoint.ToString());
        if (!::BlockIndex().count(hashCheckpoint))
            return error("%s: block index missing for received sync-checkpoint %s", __func__, hashCheckpoint.ToString());

        pindexSyncCheckpoint = ::BlockIndex()[hashSyncCheckpoint];
        pindexCheckpointRecv = ::BlockIndex()[hashCheckpoint];
    }

    if (pindexCheckpointRecv->nHeight <= pindexSyncCheckpoint->nHeight)
    {
        // Received an older checkpoint, trace back from current checkpoint
        // to the same height of the received checkpoint to verify
        // that current checkpoint should be a descendant block
        CBlockIndex* pindex = pindexSyncCheckpoint;
        while (pindex->nHeight > pindexCheckpointRecv->nHeight)
            if (!(pindex = pindex->pprev))
                return error("%s: pprev1 null - block index structure failure", __func__);
        if (pindex->GetBlockHash() != hashCheckpoint)
        {
            return error("%s: new sync-checkpoint %s is conflicting with current sync-checkpoint %s", __func__, hashCheckpoint.ToString(), hashSyncCheckpoint.ToString());
        }
        return false; // ignore older checkpoint
    }

    // Received checkpoint should be a descendant block of the current
    // checkpoint. Trace back to the same height of current checkpoint
    // to verify.
    CBlockIndex* pindex = pindexCheckpointRecv;
    while (pindex->nHeight > pindexSyncCheckpoint->nHeight)
        if (!(pindex = pindex->pprev))
            return error("%s: pprev2 null - block index structure failure", __func__);

    if (pindex->GetBlockHash() != hashSyncCheckpoint)
    {
        return error("%s: new sync-checkpoint %s is not a descendant of current sync-checkpoint %s", __func__, hashCheckpoint.ToString(), hashSyncCheckpoint.ToString());
    }
    return true;
}

bool WriteSyncCheckpoint(const uint256& hashCheckpoint)
{
    if (!pblocktree->WriteSyncCheckpoint(hashCheckpoint))
        return error("%s: failed to write to txdb sync checkpoint %s", __func__, hashCheckpoint.ToString());

    ::ChainstateActive().ForceFlushStateToDisk();
    hashSyncCheckpoint = hashCheckpoint;
    return true;
}

bool AcceptPendingSyncCheckpoint()
{
    LOCK(cs_main);
    bool havePendingCheckpoint = hashPendingCheckpoint != uint256() && ::BlockIndex().count(hashPendingCheckpoint);
    if (!havePendingCheckpoint)
        return false;

    if (!ValidateSyncCheckpoint(hashPendingCheckpoint))
    {
        hashPendingCheckpoint = uint256();
        checkpointMessagePending.SetNull();
        return false;
    }

    if (!::ChainActive().Contains(::BlockIndex()[hashPendingCheckpoint]))
        return false;

    if (!WriteSyncCheckpoint(hashPendingCheckpoint)) {
        return error("%s: failed to write sync checkpoint %s", __func__, hashPendingCheckpoint.ToString());
    }

    hashPendingCheckpoint = uint256();
    checkpointMessage = checkpointMessagePending;
    checkpointMessagePending.SetNull();

    // Relay the checkpoint
    if (g_connman && !checkpointMessage.IsNull())
    {
        g_connman->ForEachNode([](CNode* pnode) {
            if (pnode->supportACPMessages)
                checkpointMessage.RelayTo(pnode);
        });
    }

    return true;
}

// Automatically select a suitable sync-checkpoint
uint256 AutoSelectSyncCheckpoint()
{
    // Search backward for a block with specified depth policy
    const CBlockIndex *pindex = ::ChainActive().Tip();
    while (pindex->pprev && pindex->nHeight + gArgs.GetArg("-checkpointdepth", DEFAULT_AUTOCHECKPOINT) > ::ChainActive().Tip()->nHeight)
        pindex = pindex->pprev;
    return pindex->GetBlockHash();
}

// Check against synchronized checkpoint
bool CheckSyncCheckpoint(const uint256 hashBlock, const int nHeight)
{
    LOCK(cs_main);

    // Genesis block
    if (nHeight == 0) {
        return true;
    }

    // Checkpoint on default
    if (hashSyncCheckpoint == uint256()) {
        return true;
    }

    // sync-checkpoint should always be accepted block
    assert(::BlockIndex().count(hashSyncCheckpoint));
    const CBlockIndex* pindexSync = ::BlockIndex()[hashSyncCheckpoint];

    if (nHeight > pindexSync->nHeight)
    {
        // Trace back to same height as sync-checkpoint
        const CBlockIndex* pindex = ::ChainActive().Tip();
        while (pindex->nHeight > pindexSync->nHeight)
            if (!(pindex = pindex->pprev))
                return error("%s: pprev null - block index structure failure", __func__);
        if (pindex->nHeight < pindexSync->nHeight || pindex->GetBlockHash() != hashSyncCheckpoint)
            return false; // only descendant of sync-checkpoint can pass check
    }
    if (nHeight == pindexSync->nHeight && hashBlock != hashSyncCheckpoint)
        return error("%s: Same height with sync-checkpoint", __func__);
    if (nHeight < pindexSync->nHeight && !::BlockIndex().count(hashBlock))
        return error("%s: Lower height than sync-checkpoint", __func__);
    return true;
}

// Reset synchronized checkpoint to the genesis block
bool ResetSyncCheckpoint()
{
    LOCK(cs_main);

    if (!WriteSyncCheckpoint(Params().GetConsensus().hashGenesisBlock))
        return error("%s: failed to reset sync checkpoint to genesis block", __func__);

    return true;
}

// Verify sync checkpoint master pubkey and reset sync checkpoint if changed
bool CheckCheckpointPubKey()
{
    std::string strPubKey = "";
    std::string strMasterPubKey = Params().GetConsensus().checkpointPubKey;

    if (!pblocktree->ReadCheckpointPubKey(strPubKey) || strPubKey != strMasterPubKey)
    {
        // write checkpoint master key to db
        if (!ResetSyncCheckpoint())
            return error("%s: failed to reset sync-checkpoint", __func__);
        if (!pblocktree->WriteCheckpointPubKey(strMasterPubKey))
            return error("%s: failed to write new checkpoint master key to db", __func__);
        ::ChainstateActive().ForceFlushStateToDisk();
    }

    return true;
}

bool SetCheckpointPrivKey(std::string strPrivKey)
{
    CKey key = DecodeSecret(strPrivKey);
    if (!key.IsValid())
        return false;

    CSyncCheckpoint::strMasterPrivKey = strPrivKey;
    return true;
}

bool SendSyncCheckpoint(uint256 hashCheckpoint)
{
    // P2P disabled
    if (!g_connman)
        return true;

    // No connections
    if (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)
        return true;

    // Do not send dummy checkpoint
    if (hashCheckpoint == uint256())
        return true;

    CSyncCheckpoint checkpoint;
    checkpoint.hashCheckpoint = hashCheckpoint;
    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << static_cast<CUnsignedSyncCheckpoint>(checkpoint);
    checkpoint.vchMsg = std::vector<unsigned char>(sMsg.begin(), sMsg.end());

    if (CSyncCheckpoint::strMasterPrivKey.empty())
        return error("%s: Checkpoint master key unavailable.", __func__);

    CKey key = DecodeSecret(CSyncCheckpoint::strMasterPrivKey);
    if (!key.IsValid())
        return error("%s: Checkpoint master key invalid", __func__);

    if (!key.Sign(Hash(checkpoint.vchMsg.begin(), checkpoint.vchMsg.end()), checkpoint.vchSig))
        return error("%s: Unable to sign checkpoint, check private key?", __func__);

    if(!checkpoint.ProcessSyncCheckpoint())
        return error("%s: Failed to process checkpoint.", __func__);

    // Relay checkpoint
    g_connman->ForEachNode([checkpoint](CNode* pnode) {
        checkpoint.RelayTo(pnode);
    });

    return true;
}


void CUnsignedSyncCheckpoint::SetNull()
{
    nVersion = 1;
    hashCheckpoint = uint256();
}

std::string CUnsignedSyncCheckpoint::ToString() const
{
    return strprintf(
            "CSyncCheckpoint(\n"
            "    nVersion       = %d\n"
            "    hashCheckpoint = %s\n"
            ")\n",
        nVersion,
        hashCheckpoint.ToString());
}

CSyncCheckpoint::CSyncCheckpoint()
{
    SetNull();
}

void CSyncCheckpoint::SetNull()
{
    CUnsignedSyncCheckpoint::SetNull();
    vchMsg.clear();
    vchSig.clear();
}

bool CSyncCheckpoint::IsNull() const
{
    return (hashCheckpoint == uint256());
}

uint256 CSyncCheckpoint::GetHash() const
{
    return Hash(this->vchMsg.begin(), this->vchMsg.end());
}

void CSyncCheckpoint::RelayTo(CNode* pfrom) const
{
    if (g_connman && pfrom->hashCheckpointKnown != hashCheckpoint && pfrom->supportACPMessages)
    {
        pfrom->hashCheckpointKnown = hashCheckpoint;
        g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::CHECKPOINT, *this));
    }
}

// Verify signature of sync-checkpoint message
bool CSyncCheckpoint::CheckSignature()
{
    std::string strMasterPubKey = Params().GetConsensus().checkpointPubKey;
    CPubKey key(ParseHex(strMasterPubKey));
    if (!key.Verify(Hash(vchMsg.begin(), vchMsg.end()), vchSig))
        return error("%s: verify signature failed", __func__);

    // Now unserialize the data
    CDataStream sMsg(vchMsg, SER_NETWORK, PROTOCOL_VERSION);
    sMsg >> *static_cast<CUnsignedSyncCheckpoint*>(this);
    return true;
}

// Process synchronized checkpoint
bool CSyncCheckpoint::ProcessSyncCheckpoint()
{
    if (!CheckSignature())
        return false;

    LOCK(cs_main);

    if (!::BlockIndex().count(hashCheckpoint) || !::ChainActive().Contains(::BlockIndex()[hashCheckpoint]))
    {
        // We haven't received the checkpoint chain, keep the checkpoint as pending
        hashPendingCheckpoint = hashCheckpoint;
        checkpointMessagePending = *this;
        LogPrintf("%s: pending for sync-checkpoint %s\n", __func__, hashCheckpoint.ToString());

        return false;
    }

    if (!ValidateSyncCheckpoint(hashCheckpoint))
        return false;

    if (!WriteSyncCheckpoint(hashCheckpoint)) {
        return error("%s: failed to write sync checkpoint %s\n", __func__, hashCheckpoint.ToString());
    }

    checkpointMessage = *this;
    hashPendingCheckpoint = uint256();
    checkpointMessagePending.SetNull();

    return true;
}