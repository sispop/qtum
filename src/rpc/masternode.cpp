// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/activemasternode.h>
#include <masternode/masternodepayments.h>
#include <rpc/server.h>
#include <rpc/blockchain.h>
#include <node/context.h>
#include <governance/governanceclasses.h>
#include <node/blockstorage.h>
#include <evo/deterministicmns.h>
#include <net.h>
#include <validation.h>
#include <node/transaction.h>
#include <rpc/server_util.h>
#include <llmq/quorums_chainlocks.h>
using node::ReadBlockFromDisk;
using node::GetTransaction;
RPCHelpMan masternodelist();

static RPCHelpMan masternode_list()
{
    return RPCHelpMan{"masternode_list",
        "\nGet a list of masternodes in different modes. This call is identical to 'masternode list' call\n",
        {
            {"mode", RPCArg::Type::STR, RPCArg::Default{"json"}, "The mode to run list in.\n"
            "\nAvailable modes:\n"
            "  addr           - Print ip address associated with a masternode (can be additionally filtered, partial match)\n"
            "  full           - Print info in format 'status payee lastpaidtime lastpaidblock IP'\n"
            "                   (can be additionally filtered, partial match)\n"
            "  info           - Print info in format 'status payee IP'\n"
            "                   (can be additionally filtered, partial match)\n"
            "  json           - Print info in JSON format (can be additionally filtered, partial match)\n"
            "  lastpaidblock  - Print the last block height a node was paid on the network\n"
            "  lastpaidtime   - Print the last time a node was paid on the network\n"
            "  owneraddress   - Print the masternode owner Syscoin address\n"
            "  payee          - Print the masternode payout Syscoin address (can be additionally filtered,\n"
            "                   partial match)\n"
            "  pubKeyOperator - Print the masternode operator public key\n"
            "  status         - Print masternode status: ENABLED / POSE_BANNED\n"
            "                   (can be additionally filtered, partial match)\n"
            "  votingaddress  - Print the masternode voting Syscoin address\n"},     
            {"filter", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Filter results. Partial match by outpoint by default in all modes,\n"
                            "additional matches in some modes are also available.\n"},           
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternode_list", "")
            + HelpExampleRpc("masternode_list", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    node::JSONRPCRequest newRequest = request;
    newRequest.params.setArray();
    for (unsigned int i = 0; i < request.params.size(); i++) {
        newRequest.params.push_back(request.params[i]);
    }
    return masternodelist().HandleRequest(newRequest);
},
    };
} 


static RPCHelpMan masternode_connect()
{
    return RPCHelpMan{"masternode_connect",
        "\nConnect to given masternode\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of the masternode to connect."},                
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternode_connect", "")
            + HelpExampleRpc("masternode_connect", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    std::string strAddress = request.params[0].get_str();

    CService addr;
    if (!Lookup(strAddress.c_str(), addr, 0, false))
        throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Incorrect masternode address %s", strAddress));
  node::NodeContext& node = EnsureAnyNodeContext(request.context);
  if(!node.connman)
      throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    // TODO: Pass CConnman instance somehow and don't use global variable.
    node.connman->OpenMasternodeConnection(CAddress(addr, NODE_NETWORK));
    CNode* pnode = node.connman->FindNode(CAddress(addr, NODE_NETWORK));
    if (!pnode || pnode->fDisconnect)
        throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Couldn't connect to masternode %s", strAddress));

    return "successfully connected";
},
    };
} 

static RPCHelpMan masternode_count()
{
    return RPCHelpMan{"masternode_count",
        "\nGet information about number of masternodes\n",
        {              
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternode_count", "")
            + HelpExampleRpc("masternode_count", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    auto mnList = deterministicMNManager->GetListAtChainTip();
    int total = mnList.GetAllMNsCount();
    int enabled = mnList.GetValidMNsCount();

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("total", total);
    obj.pushKV("enabled", enabled);
    return obj;
},
    };
} 

UniValue GetNextMasternodeForPayment(size_t heightShift)
{
    auto mnList = deterministicMNManager->GetListAtChainTip();
    auto payees = mnList.GetProjectedMNPayees(heightShift);
    if (payees.empty())
        return "unknown";
    auto payee = payees.back();
    CScript payeeScript = payee->pdmnState->scriptPayout;

    CTxDestination payeeDest;
    ExtractDestination(payeeScript, payeeDest);

    UniValue obj(UniValue::VOBJ);

    obj.pushKV("height",        (int)(mnList.GetHeight() + heightShift));
    obj.pushKV("IP:port",       payee->pdmnState->addr.ToStringAddrPort());
    obj.pushKV("proTxHash",     payee->proTxHash.ToString());
    obj.pushKV("outpoint",      payee->collateralOutpoint.ToStringShort());
    obj.pushKV("payee",         IsValidDestination(payeeDest) ? EncodeDestination(payeeDest) : "UNKNOWN");
    return obj;
}

static RPCHelpMan masternode_winner()
{
    return RPCHelpMan{"masternode_winner",
        "\nPrint info on next masternode winner to vote for\n",
        {              
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternode_winner", "")
            + HelpExampleRpc("masternode_winner", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    return GetNextMasternodeForPayment(10);
},
    };
} 

static RPCHelpMan masternode_current()
{
    return RPCHelpMan{"masternode_current",
        "\nPrint info on current masternode winner to be paid the next block (calculated locally)\n",
        {              
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternode_current", "")
            + HelpExampleRpc("masternode_current", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    return GetNextMasternodeForPayment(1);
},
    };
} 

static RPCHelpMan masternode_status()
{
    return RPCHelpMan{"masternode_status",
        "\nPrint masternode status outputs\n",
        {              
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternode_status", "")
            + HelpExampleRpc("masternode_status", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    if (!fMasternodeMode)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "This is not a masternode");

    UniValue mnObj(UniValue::VOBJ);

    CDeterministicMNCPtr dmn;
    {
        LOCK(activeMasternodeInfoCs);

        // keep compatibility with legacy status for now (might get deprecated/removed later)
        mnObj.pushKV("outpoint", activeMasternodeInfo.outpoint.ToStringShort());
        mnObj.pushKV("service", activeMasternodeInfo.service.ToStringAddrPort());
        auto mnList = deterministicMNManager->GetListAtChainTip();
        auto dmn = mnList.GetMN(activeMasternodeInfo.proTxHash);
        if (dmn) {
            mnObj.pushKV("proTxHash", dmn->proTxHash.ToString());
            mnObj.pushKV("collateralHash", dmn->collateralOutpoint.hash.ToString());
            mnObj.pushKV("collateralIndex", (int)dmn->collateralOutpoint.n);
            UniValue stateObj;
            dmn->pdmnState->ToJson(stateObj);
            mnObj.pushKV("dmnState", stateObj);
        }
    }
    mnObj.pushKV("state", activeMasternodeManager->GetStateString());
    mnObj.pushKV("status", activeMasternodeManager->GetStatus());

    return mnObj;
},
    };
} 
std::string GetRequiredPaymentsString(int nBlockHeight, const CDeterministicMNCPtr &payee)
{
    std::string strPayments = "Unknown";
    if (payee) {
        CTxDestination dest;
        if (!ExtractDestination(payee->pdmnState->scriptPayout, dest)) {
            CHECK_NONFATAL(false);
        }
        strPayments = EncodeDestination(dest);
        if (payee->nOperatorReward != 0 && payee->pdmnState->scriptOperatorPayout != CScript()) {
            if (!ExtractDestination(payee->pdmnState->scriptOperatorPayout, dest)) {
                CHECK_NONFATAL(false);
            }
            strPayments += ", " + EncodeDestination(dest);
        }
    }
    if (CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
        std::vector<CTxOut> voutSuperblock;
        if (!CSuperblockManager::GetSuperblockPayments(nBlockHeight, voutSuperblock)) {
            return strPayments + ", error";
        }
        std::string strSBPayees = "Unknown";
        for (const auto& txout : voutSuperblock) {
            CTxDestination dest;
            ExtractDestination(txout.scriptPubKey, dest);
            if (strSBPayees != "Unknown") {
                strSBPayees += ", " + EncodeDestination(dest);
            } else {
                strSBPayees = EncodeDestination(dest);
            }
        }
        strPayments += ", " + strSBPayees;
    }
    return strPayments;
}


static RPCHelpMan masternode_sign()
{
    return RPCHelpMan{"masternode_sign",
        "\nSign BLS message against previous CL + msghash using MN operator BLS key. Must be in winners list as well.\n",
        {         
            {"msghash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "msg hash."}, 
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
            HelpExampleCli("masternode_sign", "")
    + HelpExampleRpc("masternode_sign", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    uint256 msgHash = ParseHashV(request.params[0], "msgHash");
    CBLSSignature sig;
    const CBlockIndex* pindexTip{nullptr};
    const CBlockIndex* prevCLIndex = llmq::chainLocksHandler->GetPreviousChainLock();
    if (!prevCLIndex) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No previous chainlock found");
    }
    {
        LOCK(cs_main);
        pindexTip = node.chainman->ActiveChain()[prevCLIndex->nHeight];
        if(pindexTip->GetBlockHash() != prevCLIndex->GetBlockHash()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Invalid previous chainlock");
        }
    }
    int nCount = 10;
    int nChainTipHeight = pindexTip->nHeight;
    int nStartHeight = std::max(nChainTipHeight - nCount, 1);
    bool bFoundActiveMN{false};
    auto blsPubKeyOperator = WITH_LOCK(activeMasternodeInfoCs, return *(activeMasternodeInfo.blsPubKeyOperator.get()));
    for (int h = nStartHeight; h <= nChainTipHeight; h++) {
        const auto &payee = deterministicMNManager->GetListForBlock(pindexTip->GetAncestor(h - 1)).GetMNPayee();
        if(payee->pdmnState->pubKeyOperator.Get() == blsPubKeyOperator) {
            bFoundActiveMN = true;
            break;
        }
    }
    if(!bFoundActiveMN) {
        const auto &mnList = deterministicMNManager->GetListForBlock(pindexTip);
        const auto &projection = mnList.GetProjectedMNPayees(nCount*2);
        for (size_t i = 0; i < projection.size(); i++) {
            const auto &payee = projection[i];
            if(payee->pdmnState->pubKeyOperator.Get() == blsPubKeyOperator) {
                bFoundActiveMN = true;
                break;
            }
        }
    }
    if(!bFoundActiveMN) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "MN operator not in winners list");
    }
    {
        LOCK(activeMasternodeInfoCs);
        if (!activeMasternodeInfo.blsKeyOperator) {
             throw JSONRPCError(RPC_INTERNAL_ERROR, "No MN operator BLS operator key found");
        }
        if (activeMasternodeInfo.blsKeyOperator) {
            sig = activeMasternodeInfo.blsKeyOperator->Sign(msgHash);
        }
    }
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("signature", sig.ToString());
    obj.pushKV("blspubkey", blsPubKeyOperator.ToString());
    return obj;
},
    };
}

static RPCHelpMan masternode_verify()
{
    return RPCHelpMan{"masternode_verify",
        "\nVerify BLS message signed against previous CL + msghash using MN operator BLS key. Must be in winners list as well.\n",
        {         
            {"msghash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "msg hash"},
            {"signature", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "signature"},
            {"blspubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "BLS MN operator pubkey"},
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
            HelpExampleCli("masternode_verify", "")
    + HelpExampleRpc("masternode_verify", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    uint256 msgHash = ParseHashV(request.params[0], "msgHash");
    CBLSSignature sig;
    if (!sig.SetHexStr(request.params[1].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid signature format");
    }
    CBLSPublicKey blsPubKeyOperator;
    if (!blsPubKeyOperator.SetHexStr(request.params[2].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid operator pubkey");
    }
    if(!sig.VerifyInsecure(blsPubKeyOperator, msgHash)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Signature validation failed");
    }
    const CBlockIndex* pindexTip{nullptr};
    const CBlockIndex* prevCLIndex = llmq::chainLocksHandler->GetPreviousChainLock();
    if (!prevCLIndex) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No previous chainlock found");
    }
    {
        LOCK(cs_main);
        pindexTip = node.chainman->ActiveChain()[prevCLIndex->nHeight];
        if(pindexTip->GetBlockHash() != prevCLIndex->GetBlockHash()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Invalid previous chainlock");
        }
    }
    int nCount = 10;
    int nChainTipHeight = pindexTip->nHeight;
    int nStartHeight = std::max(nChainTipHeight - nCount, 1);
    bool bFoundActiveMN{false};
    for (int h = nStartHeight; h <= nChainTipHeight; h++) {
        const auto &payee = deterministicMNManager->GetListForBlock(pindexTip->GetAncestor(h - 1)).GetMNPayee();
        if(payee->pdmnState->pubKeyOperator.Get() == blsPubKeyOperator) {
            bFoundActiveMN = true;
            break;
        }
    }
    if(!bFoundActiveMN) {
        const auto &mnList = deterministicMNManager->GetListForBlock(pindexTip);
        const auto &projection = mnList.GetProjectedMNPayees(nCount*2);
        for (size_t i = 0; i < projection.size(); i++) {
            const auto &payee = projection[i];
            if(payee->pdmnState->pubKeyOperator.Get() == blsPubKeyOperator) {
                bFoundActiveMN = true;
                break;
            }
        }
    }
    if(!bFoundActiveMN) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "MN operator not in winners list");
    }
    return "success";
},
    };
}

static RPCHelpMan masternode_winners()
{
    return RPCHelpMan{"masternode_winners",
        "\nPrint list of masternode winners\n",
        {         
            {"count", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Number of last winners to return."}, 
            {"filter", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Filter for returned winners."},
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternode_winners", "")
            + HelpExampleRpc("masternode_winners", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    const CBlockIndex* pindexTip{nullptr};
    {
        LOCK(cs_main);
        pindexTip = node.chainman->ActiveTip();
        if (!pindexTip) return NullUniValue;
    }

    int nCount = 10;
    std::string strFilter;

    if (!request.params[0].isNull()) {
        nCount = request.params[0].getInt<int>();
    }

    if (!request.params[1].isNull()) {
        strFilter = request.params[1].get_str();
    }

    UniValue obj(UniValue::VOBJ);
    int nChainTipHeight = pindexTip->nHeight;
    int nStartHeight = std::max(nChainTipHeight - nCount, 1);

    for (int h = nStartHeight; h <= nChainTipHeight; h++) {
        auto payee = deterministicMNManager->GetListForBlock(pindexTip->GetAncestor(h - 1)).GetMNPayee();
        std::string strPayments = GetRequiredPaymentsString(h, payee);
        if (strFilter != "" && strPayments.find(strFilter) == std::string::npos) continue;
        obj.pushKV(strprintf("%d", h), strPayments);
    }
    auto projection = deterministicMNManager->GetListForBlock(pindexTip).GetProjectedMNPayees(20);
    for (size_t i = 0; i < projection.size(); i++) {
        int h = nChainTipHeight + 1 + i;
        std::string strPayments = GetRequiredPaymentsString(h, projection[i]);
        if (strFilter != "" && strPayments.find(strFilter) == std::string::npos) continue;
        obj.pushKV(strprintf("%d", h), strPayments);
    }
    return obj;
},
    };
} 

RPCHelpMan masternode_payments()
{
    return RPCHelpMan{"masternode_payments",
        "\nReturns an array of deterministic masternodes and their payments for the specified block\n",
        {         
            {"blockhash", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The hash of the starting block."},
            {"count", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The number of blocks to return.\n"
                                                                    "Will return <count> previous blocks if <count> is negative.\n"
                                                                    "Both 1 and -1 correspond to the chain tip."},    
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternode_payments", "")
            + HelpExampleRpc("masternode_payments", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    CBlockIndex* pindex{nullptr};
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    if (request.params[0].isNull()) {
        LOCK(cs_main);
        pindex = node.chainman->ActiveTip();
    } else {
        LOCK(cs_main);
        uint256 blockHash = ParseHashV(request.params[0], "blockhash");
        pindex = node.chainman->m_blockman.LookupBlockIndex(blockHash);
        if (pindex == nullptr) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }

    int64_t nCount = request.params.size() > 1 ? request.params[1].getInt<int64_t>() : 1;

    // A temporary vector which is used to sort results properly (there is no "reverse" in/for UniValue)
    std::vector<UniValue> vecPayments;

    while (vecPayments.size() < (size_t)std::abs(nCount) && pindex != nullptr) {

        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");
        }

        // Note: we have to actually calculate block reward from scratch instead of simply querying coinbase vout
        // because miners might collect less coins than they potentially could and this would break our calculations.
        CAmount nBlockFees{0};
        for (const auto& tx : block.vtx) {
            if (tx->IsCoinBase()) {
                continue;
            }
            CAmount nValueIn{0};
            for (const auto &txin : tx->vin) {
                uint256 blockHashTmp;
                CTransactionRef txPrev = GetTransaction( pindex, node.mempool.get(), txin.prevout.hash, Params().GetConsensus(), blockHashTmp);
                nValueIn += txPrev->vout[txin.prevout.n].nValue;
            }
            nBlockFees += nValueIn - tx->GetValueOut();
        }

        std::vector<CTxOut> voutMasternodePayments, voutDummy;
        CAmount blockReward = nBlockFees + GetBlockSubsidy(pindex->pprev->nHeight, Params().GetConsensus());
        CMutableTransaction coinbaseTx;
        coinbaseTx.vout.resize(1);
        coinbaseTx.vout[0].nValue = blockReward + nBlockFees;
        FillBlockPayments(WITH_LOCK(node.chainman->GetMutex(), return node.chainman->ActiveChain()), coinbaseTx, pindex->nHeight, blockReward, nBlockFees, voutMasternodePayments, voutDummy);

        UniValue blockObj(UniValue::VOBJ);
        CAmount payedPerBlock{0};

        UniValue masternodeArr(UniValue::VARR);
        UniValue protxObj(UniValue::VOBJ);
        UniValue payeesArr(UniValue::VARR);
        CAmount payedPerMasternode{0};

        for (const auto& txout : voutMasternodePayments) {
            UniValue obj(UniValue::VOBJ);
            CTxDestination dest;
            ExtractDestination(txout.scriptPubKey, dest);
            obj.pushKV("address", EncodeDestination(dest));
            obj.pushKV("script", HexStr(txout.scriptPubKey));
            obj.pushKV("amount", txout.nValue);
            payedPerMasternode += txout.nValue;
            payeesArr.push_back(obj);
        }

        // NOTE: we use _previous_ block to find a payee for the current one
        const auto dmnPayee = deterministicMNManager->GetListForBlock(pindex->pprev).GetMNPayee();
        protxObj.pushKV("proTxHash", dmnPayee == nullptr ? "" : dmnPayee->proTxHash.ToString());
        protxObj.pushKV("amount", payedPerMasternode);
        protxObj.pushKV("payees", payeesArr);
        payedPerBlock += payedPerMasternode;
        masternodeArr.push_back(protxObj);

        blockObj.pushKV("height", pindex->nHeight);
        blockObj.pushKV("blockhash", pindex->GetBlockHash().ToString());
        blockObj.pushKV("amount", payedPerBlock);
        blockObj.pushKV("masternodes", masternodeArr);
        vecPayments.push_back(blockObj);

        if (nCount > 0) {
            LOCK(cs_main);
            pindex = node.chainman->ActiveChain().Next(pindex);
        } else {
            pindex = pindex->pprev;
        }
    }

    if (nCount < 0) {
        std::reverse(vecPayments.begin(), vecPayments.end());
    }

    UniValue paymentsArr(UniValue::VARR);
    for (const auto& payment : vecPayments) {
        paymentsArr.push_back(payment);
    }

    return paymentsArr;
},
    };
} 


RPCHelpMan masternodelist()
{
    return RPCHelpMan{"masternodelist",
        "\nPrint list of masternode list\n",
        {         
            {"mode", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Mode."},
            {"filter", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "filter."},    
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{
                HelpExampleCli("masternodelist", "")
            + HelpExampleRpc("masternodelist", "")
        },
    [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    const node::NodeContext& node = EnsureAnyNodeContext(request.context);
    std::string strMode = "json";
    std::string strFilter;

    if (!request.params[0].isNull()) strMode = request.params[0].get_str();
    if (!request.params[1].isNull()) strFilter = request.params[1].get_str();
    strMode = ToLower(strMode);

    if (strMode != "addr" && strMode != "full" && strMode != "info" && strMode != "json" &&
                strMode != "owneraddress" && strMode != "votingaddress" &&
                strMode != "lastpaidtime" && strMode != "lastpaidblock" &&
                strMode != "payee" && strMode != "pubkeyoperator" &&
                strMode != "status")
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
    }

    UniValue obj(UniValue::VOBJ);
    auto mnList = deterministicMNManager->GetListAtChainTip();
    auto dmnToStatus = [&](auto& dmn) {
        if (mnList.IsMNValid(dmn)) {
            return "ENABLED";
        }
        if (mnList.IsMNPoSeBanned(dmn)) {
            return "POSE_BANNED";
        }
        return "UNKNOWN";
    };
    auto dmnToLastPaidTime = [&](auto& dmn) {
        if (dmn.pdmnState->nLastPaidHeight == 0) {
            return (int)0;
        }

        LOCK(cs_main);
        const CBlockIndex* pindex = node.chainman->ActiveChain()[dmn.pdmnState->nLastPaidHeight];
        return (int)pindex->nTime;
    };

    mnList.ForEachMN(false, [&](auto& dmn) {
        std::string strOutpoint = dmn.collateralOutpoint.ToStringShort();
        std::string collateralAddressStr = "UNKNOWN";
        std::map<COutPoint, Coin> coins;
        coins[dmn.collateralOutpoint]; 
        node.chain->findCoins(coins);
        const Coin &coin = coins.at(dmn.collateralOutpoint);
        if (!coin.IsSpent()) {
            CTxDestination collateralDest;
            if (ExtractDestination(coin.out.scriptPubKey, collateralDest)) {
                collateralAddressStr = EncodeDestination(collateralDest);
            }
        }

        CScript payeeScript = dmn.pdmnState->scriptPayout;
        CTxDestination payeeDest;
        std::string payeeStr = "UNKNOWN";
        if (ExtractDestination(payeeScript, payeeDest)) {
            payeeStr = EncodeDestination(payeeDest);
        }

        if (strMode == "addr") {
            std::string strAddress = dmn.pdmnState->addr.ToStringAddrPort();
            if (strFilter !="" && strAddress.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, strAddress);
        } else if (strMode == "full") {
            std::ostringstream streamFull;
            streamFull << std::setw(18) <<
                           dmnToStatus(dmn) << " " <<
                           payeeStr << " " << std::setw(10) <<
                           dmnToLastPaidTime(dmn) << " "  << std::setw(6) <<
                           dmn.pdmnState->nLastPaidHeight << " " <<
                           dmn.pdmnState->addr.ToStringAddrPort();
            std::string strFull = streamFull.str();
            if (strFilter !="" && strFull.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, strFull);
        } else if (strMode == "info") {
            std::ostringstream streamInfo;
            streamInfo << std::setw(18) <<
                           dmnToStatus(dmn) << " " <<
                           payeeStr << " " <<
                           dmn.pdmnState->addr.ToStringAddrPort();
            std::string strInfo = streamInfo.str();
            if (strFilter !="" && strInfo.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, strInfo);
        } else if (strMode == "json") {
            std::ostringstream streamInfo;
            streamInfo <<  dmn.proTxHash.ToString() << " " <<
                           dmn.pdmnState->addr.ToStringAddrPort() << " " <<
                           payeeStr << " " <<
                           dmnToStatus(dmn) << " " <<
                           dmnToLastPaidTime(dmn) << " " <<
                           dmn.pdmnState->nLastPaidHeight << " " <<
                           EncodeDestination(WitnessV0KeyHash(dmn.pdmnState->keyIDOwner)) << " " <<
                           EncodeDestination(WitnessV0KeyHash(dmn.pdmnState->keyIDVoting)) << " " <<
                           collateralAddressStr << " " <<
                           dmn.pdmnState->pubKeyOperator.Get().ToString();
            std::string strInfo = streamInfo.str();
            if (strFilter !="" && strInfo.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) return;
            UniValue objMN(UniValue::VOBJ);
            objMN.pushKV("proTxHash", dmn.proTxHash.ToString());
            objMN.pushKV("address", dmn.pdmnState->addr.ToStringAddrPort());
            objMN.pushKV("payee", payeeStr);
            objMN.pushKV("status", dmnToStatus(dmn));
            objMN.pushKV("collateralblock", dmn.pdmnState->nCollateralHeight);
            objMN.pushKV("lastpaidtime", dmnToLastPaidTime(dmn));
            objMN.pushKV("lastpaidblock", dmn.pdmnState->nLastPaidHeight);
            objMN.pushKV("owneraddress", EncodeDestination(WitnessV0KeyHash(dmn.pdmnState->keyIDOwner)));
            objMN.pushKV("votingaddress", EncodeDestination(WitnessV0KeyHash(dmn.pdmnState->keyIDVoting)));
            objMN.pushKV("collateraladdress", collateralAddressStr);
            objMN.pushKV("pubkeyoperator", dmn.pdmnState->pubKeyOperator.Get().ToString());
            obj.pushKV(strOutpoint, objMN);
        } else if (strMode == "lastpaidblock") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, dmn.pdmnState->nLastPaidHeight);
        } else if (strMode == "lastpaidtime") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, dmnToLastPaidTime(dmn));
        } else if (strMode == "payee") {
            if (strFilter !="" && payeeStr.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, payeeStr);
        } else if (strMode == "owneraddress") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, EncodeDestination(WitnessV0KeyHash(dmn.pdmnState->keyIDOwner)));
        } else if (strMode == "pubkeyoperator") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, dmn.pdmnState->pubKeyOperator.Get().ToString());
        } else if (strMode == "status") {
            std::string strStatus = dmnToStatus(dmn);
            if (strFilter !="" && strStatus.find(strFilter) == std::string::npos &&
                strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, strStatus);
        } else if (strMode == "votingaddress") {
            if (strFilter !="" && strOutpoint.find(strFilter) == std::string::npos) return;
            obj.pushKV(strOutpoint, EncodeDestination(WitnessV0KeyHash(dmn.pdmnState->keyIDVoting)));
        }
    });

    return obj;
},
    };
} 

void RegisterMasternodeRPCCommands(CRPCTable &t)
{
    static const CRPCCommand commands[]{
        {"masternode", &masternode_connect},
        {"masternode", &masternode_list},
        {"masternode", &masternode_winners},
        {"masternode", &masternode_payments},
        {"masternode", &masternode_count},
        {"masternode", &masternode_winner},
        {"masternode", &masternode_status},
        {"masternode", &masternode_current},
        {"masternode", &masternode_sign},
        {"masternode", &masternode_verify},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
