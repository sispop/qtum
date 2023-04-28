// Copyright (c) 2018-2020 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SYSCOIN_LLMQ_QUORUMS_DKGSESSIONHANDLER_H
#define SYSCOIN_LLMQ_QUORUMS_DKGSESSIONHANDLER_H


#include <ctpl_stl.h>
#include <net.h>

class CBLSWorker;
class CBlockIndex;
class CConnman;
class PeerManager;
class ChainstateManager;
namespace llmq
{
class CDKGSession;
class CDKGSessionManager;
enum QuorumPhase {
    QuorumPhase_None = -1,
    QuorumPhase_Initialized = 1,
    QuorumPhase_Contribute,
    QuorumPhase_Complain,
    QuorumPhase_Justify,
    QuorumPhase_Commit,
    QuorumPhase_Finalize,
    QuorumPhase_Idle,
};

/**
 * Acts as a FIFO queue for incoming DKG messages. The reason we need this is that deserialization of these messages
 * is too slow to be processed in the main message handler thread. So, instead of processing them directly from the
 * main handler thread, we push them into a CDKGPendingMessages object and later pop+deserialize them in the DKG phase
 * handler thread.
 *
 * Each message type has it's own instance of this class.
 */
class CDKGPendingMessages
{
public:
    using BinaryMessage = std::pair<NodeId, std::shared_ptr<CDataStream>>;

private:
    mutable RecursiveMutex cs;
    size_t maxMessagesPerNode GUARDED_BY(cs);
    std::list<BinaryMessage> pendingMessages GUARDED_BY(cs);
    std::map<NodeId, size_t> messagesPerNode GUARDED_BY(cs);
    std::set<uint256> seenMessages GUARDED_BY(cs);

public:
    PeerManager& peerman;
    explicit CDKGPendingMessages(size_t _maxMessagesPerNode, PeerManager& _peerman): maxMessagesPerNode(_maxMessagesPerNode), peerman(_peerman) {};

    void PushPendingMessage(CNode* from, CDataStream& vRecv);
    std::list<BinaryMessage> PopPendingMessages(size_t maxCount);
    bool HasSeen(const uint256& hash) const;
    void Clear();

    template<typename Message>
    void PushPendingMessage(CNode* from, Message& msg)
    {
        CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
        ds << msg;
        PushPendingMessage(from, ds);
    }

    // Might return nullptr messages, which indicates that deserialization failed for some reason
    template<typename Message>
    std::vector<std::pair<NodeId, std::shared_ptr<Message>>> PopAndDeserializeMessages(size_t maxCount)
    {
        const auto &binaryMessages = PopPendingMessages(maxCount);
        if (binaryMessages.empty()) {
            return {};
        }

        std::vector<std::pair<NodeId, std::shared_ptr<Message>>> ret;
        ret.reserve(binaryMessages.size());
        for (const auto& bm : binaryMessages) {
            auto msg = std::make_shared<Message>();
            try {
                *bm.second >> *msg;
            } catch (...) {
                msg = nullptr;
            }
            ret.emplace_back(std::make_pair(bm.first, std::move(msg)));
        }

        return ret;
    }
};

/**
 * Handles multiple sequential sessions of one specific LLMQ type. There is one instance of this class per LLMQ type.
 *
 * It internally starts the phase handler thread, which constantly loops and sequentially processes one session at a
 * time and waiting for the next phase if necessary.
 */
class CDKGSessionHandler
{
private:
    friend class CDKGSessionManager;

private:
    mutable RecursiveMutex cs;
    std::atomic<bool> stopRequested{false};

    const Consensus::LLMQParams& params;
    CBLSWorker& blsWorker;
    CDKGSessionManager& dkgManager;
    ChainstateManager& chainman;

    QuorumPhase phase GUARDED_BY(cs) {QuorumPhase_Idle};
    int currentHeight GUARDED_BY(cs) {-1};
    uint256 quorumHash GUARDED_BY(cs);

    std::unique_ptr<CDKGSession> curSession;
    std::thread phaseHandlerThread;

    CDKGPendingMessages pendingContributions GUARDED_BY(cs);
    CDKGPendingMessages pendingComplaints GUARDED_BY(cs);
    CDKGPendingMessages pendingJustifications GUARDED_BY(cs);
    CDKGPendingMessages pendingPrematureCommitments GUARDED_BY(cs);
    std::string m_threadName;
    PeerManager& peerman;
public:
    CDKGSessionHandler(const Consensus::LLMQParams& _params, CBLSWorker& blsWorker, CDKGSessionManager& _dkgManager, PeerManager& peerman, ChainstateManager& _chainman);
    ~CDKGSessionHandler() = default;

    void UpdatedBlockTip(const CBlockIndex *pindexNew);
    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv);

    void StartThread();
    void StopThread();
    const char* GetName() {return m_threadName.c_str();}

private:
    bool InitNewQuorum(const CBlockIndex* pQuorumBaseBlockIndex);

    std::pair<QuorumPhase, uint256> GetPhaseAndQuorumHash() const;

    using StartPhaseFunc = std::function<void()>;
    using WhileWaitFunc = std::function<bool()>;
    void WaitForNextPhase(QuorumPhase curPhase, QuorumPhase nextPhase, const uint256& expectedQuorumHash, const WhileWaitFunc& runWhileWaiting) const;
    void WaitForNewQuorum(const uint256& oldQuorumHash) const;
    void SleepBeforePhase(QuorumPhase curPhase, const uint256& expectedQuorumHash, double randomSleepFactor, const WhileWaitFunc& runWhileWaiting) const;
    void HandlePhase(QuorumPhase curPhase, QuorumPhase nextPhase, const uint256& expectedQuorumHash, double randomSleepFactor, const StartPhaseFunc& startPhaseFunc, const WhileWaitFunc& runWhileWaiting);
    void HandleDKGRound();
    void PhaseHandlerThread();
};

} // namespace llmq

#endif // SYSCOIN_LLMQ_QUORUMS_DKGSESSIONHANDLER_H
