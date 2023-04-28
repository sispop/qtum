// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <httpserver.h>
#include <index/blockfilterindex.h>
#include <index/coinstatsindex.h>
#include <index/txindex.h>
#include <interfaces/chain.h>
#include <interfaces/echo.h>
#include <interfaces/init.h>
#include <interfaces/ipc.h>
#include <key_io.h>
#include <node/context.h>
#include <outputtype.h>
#include <rpc/blockchain.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <scheduler.h>
#include <script/descriptor.h>
#include <util/check.h>
#include <util/message.h> // For MessageSign(), MessageVerify()
#include <util/strencodings.h>
#include <util/syscall_sandbox.h>
#include <util/system.h>

#include <txmempool.h>
#include <masternode/masternodesync.h>
#include <spork.h>
#include <bls/bls.h>
#include <llmq/quorums_utils.h>
#include <validation.h>

#include <optional>
#include <stdint.h>
#include <tuple>
#ifdef HAVE_MALLOC_INFO
#include <malloc.h>
#endif

#include <univalue.h>

using node::NodeContext;

static RPCHelpMan mnsync()
{
        return RPCHelpMan{"mnsync",
            {"\nReturns the sync status, updates to the next step or resets it entirely.\n"},
            {
                {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "The command to issue (status|next|reset)"},
            },
            RPCResult{RPCResult::Type::ANY, "result", "Result"},
            RPCExamples{
                HelpExampleCli("mnsync", "status")
                + HelpExampleRpc("mnsync", "status")
            },
        [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{

    NodeContext& node = EnsureAnyNodeContext(request.context);
    std::string strMode = request.params[0].get_str();

    if(strMode == "status") {
        UniValue objStatus(UniValue::VOBJ);
        objStatus.pushKV("AssetID", masternodeSync.GetAssetID());
        objStatus.pushKV("AssetName", masternodeSync.GetAssetName());
        objStatus.pushKV("AssetStartTime", masternodeSync.GetAssetStartTime());
        objStatus.pushKV("Attempt", masternodeSync.GetAttempt());
        objStatus.pushKV("IsBlockchainSynced", masternodeSync.IsBlockchainSynced());
        objStatus.pushKV("IsSynced", masternodeSync.IsSynced());
        return objStatus;
    }

    if(strMode == "next")
    {
        if (!node.connman)
            throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

        masternodeSync.SwitchToNextAsset(*node.connman);
        return "sync updated to " + masternodeSync.GetAssetName();
    }

    if(strMode == "reset")
    {
        masternodeSync.Reset(true);
        return "success";
    }
    return "failure";
},
    };
}

/*
    Used for updating/reading spork settings on the network
*/
static RPCHelpMan spork()
{
    return RPCHelpMan{"spork",
            {"\nShows or updates the value of the specific spork. Requires \"-sporkkey\" to be set to sign the message for updating.\n"},
            {
                {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "\"show\" to show all current spork values, \"active\" to show which sporks are active or the name of the spork to update"},
                {"value", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The new desired value of the spork if updating"},
            },
            {
                RPCResult{"for command = \"show\"",
                    RPCResult::Type::ANY, "SPORK_NAME", "The value of the specific spork with the name SPORK_NAME"},
                RPCResult{"for command = \"active\"",
                    RPCResult::Type::ANY, "SPORK_NAME", "'true' for time-based sporks if spork is active and 'false' otherwise"},
                RPCResult{"for updating",
                    RPCResult::Type::ANY, "result", "\"success\" if spork value was updated or this help otherwise"},
            },
            RPCExamples{
                HelpExampleCli("spork", "SPORK_9_NEW_SIGS 4070908800") 
                + HelpExampleCli("spork", "show")
                + HelpExampleRpc("spork", "\"SPORK_9_NEW_SIGS\", 4070908800")
                + HelpExampleRpc("spork", "\"show\"")
            },
        [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    std::string strCommand = request.params[0].get_str();
    if(strCommand != "show" && strCommand != "active") {
        NodeContext& node = EnsureAnyNodeContext(request.context);
        // advanced mode, update spork values
        int nSporkID = CSporkManager::GetSporkIDByName(request.params[0].get_str());
        if(nSporkID == SPORK_INVALID)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid spork name");

        if (!node.connman)
            throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

        // SPORK VALUE
        int64_t nValue = request.params[1].getInt<int64_t>();;

        //broadcast new spork
        if(sporkManager.UpdateSpork(nSporkID, nValue, *node.peerman)){
            return "success";
        }
    } else {
        // basic mode, show info
        if (strCommand == "show") {
            UniValue ret(UniValue::VOBJ);
            for (const auto& sporkDef : sporkDefs) {
                ret.pushKV(std::string(sporkDef.name), sporkManager.GetSporkValue(sporkDef.sporkId));
            }
            return ret;
        } else if(strCommand == "active"){
            UniValue ret(UniValue::VOBJ);
            for (const auto& sporkDef : sporkDefs) {
                ret.pushKV(std::string(sporkDef.name), sporkManager.IsSporkActive(sporkDef.sporkId));
            }
            return ret;
        }
    }
    return "failure";
},
    };
}



static RPCHelpMan mnauth()
{
    return RPCHelpMan{"mnauth",
            {"\nOverride MNAUTH processing results for the specified node with a user provided data (-regtest only).\n"},
            {
                {"nodeId", RPCArg::Type::NUM, RPCArg::Optional::NO, "Internal peer id of the node the mock data gets added to"},
                {"proTxHash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The authenticated proTxHash as hex string"},
                {"publicKey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The authenticated public key as hex string"},
            },
            RPCResult{
                RPCResult::Type::BOOL, "", "If MNAUTH was overridden or not."
            },
            RPCExamples{
                "Override MNAUTH processing\n" +
                HelpExampleCli("mnauth", "\"nodeId \"proTxHash\" \"publicKey\"\"")
            },
        [&](const RPCHelpMan& self, const node::JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    if(!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    if (!Params().MineBlocksOnDemand())
        throw std::runtime_error("mnauth for regression testing (-regtest mode) only");
    auto& chainman = EnsureAnyChainman(request.context);
    int nodeId = request.params[0].getInt<int>();
    uint256 proTxHash = ParseHashV(request.params[1], "proTxHash");
    if (proTxHash.IsNull()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "proTxHash invalid");
    }
    CBLSPublicKey publicKey;
    int nHeight = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveHeight());
    bool bls_legacy_scheme = !llmq::CLLMQUtils::IsV19Active(nHeight);
    publicKey.SetHexStr(request.params[2].get_str(), bls_legacy_scheme);
    if (!publicKey.IsValid()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "publicKey invalid");
    }

    bool fSuccess = node.connman->ForNode(nodeId, AllNodes, [&](CNode* pNode){
        pNode->SetVerifiedProRegTxHash(proTxHash);
        pNode->SetVerifiedPubKeyHash(publicKey.GetHash());
        return true;
    });

    return fSuccess;
},
    };
}
static RPCHelpMan validateaddress()
{
    return RPCHelpMan{
        "validateaddress",
        "\nReturn information about the given qtum address.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The qtum address to validate"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "isvalid", "If the address is valid or not"},
                {RPCResult::Type::STR, "address", /*optional=*/true, "The qtum address validated"},
                {RPCResult::Type::STR_HEX, "scriptPubKey", /*optional=*/true, "The hex-encoded scriptPubKey generated by the address"},
                {RPCResult::Type::BOOL, "isscript", /*optional=*/true, "If the key is a script"},
                {RPCResult::Type::BOOL, "iswitness", /*optional=*/true, "If the address is a witness address"},
                {RPCResult::Type::NUM, "witness_version", /*optional=*/true, "The version number of the witness program"},
                {RPCResult::Type::STR_HEX, "witness_program", /*optional=*/true, "The hex value of the witness program"},
                {RPCResult::Type::STR, "error", /*optional=*/true, "Error message, if any"},
                {RPCResult::Type::ARR, "error_locations", /*optional=*/true, "Indices of likely error locations in address, if known (e.g. Bech32 errors)",
                    {
                        {RPCResult::Type::NUM, "index", "index of a potential error"},
                    }},
            }
        },
        RPCExamples{
            HelpExampleCli("validateaddress", "\"" + EXAMPLE_ADDRESS[0] + "\"") +
            HelpExampleRpc("validateaddress", "\"" + EXAMPLE_ADDRESS[0] + "\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string error_msg;
    std::vector<int> error_locations;
    CTxDestination dest = DecodeDestination(request.params[0].get_str(), error_msg, &error_locations);
    const bool isValid = IsValidDestination(dest);
    CHECK_NONFATAL(isValid == error_msg.empty());

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("isvalid", isValid);
    if (isValid) {
        std::string currentAddress = EncodeDestination(dest);
        ret.pushKV("address", currentAddress);

        CScript scriptPubKey = GetScriptForDestination(dest);
        ret.pushKV("scriptPubKey", HexStr(scriptPubKey));

        UniValue detail = DescribeAddress(dest);
        ret.pushKVs(detail);
    } else {
        UniValue error_indices(UniValue::VARR);
        for (int i : error_locations) error_indices.push_back(i);
        ret.pushKV("error_locations", error_indices);
        ret.pushKV("error", error_msg);
    }

    return ret;
},
    };
}

/////////////////////////////////////////////////////////////////////// // qtum
RPCHelpMan getdgpinfo()
{
    return RPCHelpMan{"getdgpinfo",
                "\nReturns an object containing DGP state info.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "maxblocksize", "Current maximum block size"},
                        {RPCResult::Type::NUM, "mingasprice", "Current minimum gas price"},
                        {RPCResult::Type::NUM, "blockgaslimit", "Current block gas limit"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getdgpinfo", "")
            + HelpExampleRpc("getdgpinfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);

    CChain& active_chain = chainman.ActiveChain();
    QtumDGP qtumDGP(globalState.get(), chainman.ActiveChainstate());

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("maxblocksize", (uint64_t)qtumDGP.getBlockSize(active_chain.Height()));
    obj.pushKV("mingasprice", (uint64_t)qtumDGP.getMinGasPrice(active_chain.Height()));
    obj.pushKV("blockgaslimit", (uint64_t)qtumDGP.getBlockGasLimit(active_chain.Height()));

    return obj;
},
    };
}

bool getAddressesFromParams(const UniValue& params, std::vector<std::pair<uint256, int> > &addresses)
{
    if (params[0].isStr()) {
        uint256 hashBytes;
        int type = 0;
        if (!DecodeIndexKey(params[0].get_str(), hashBytes, type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
        }
        addresses.push_back(std::make_pair(hashBytes, type));
    } else if (params[0].isObject()) {

        UniValue addressValues = find_value(params[0].get_obj(), "addresses");
        if (!addressValues.isArray()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Addresses is expected to be an array");
        }

        std::vector<UniValue> values = addressValues.getValues();

        for (std::vector<UniValue>::iterator it = values.begin(); it != values.end(); ++it) {

            uint256 hashBytes;
            int type = 0;
            if (!DecodeIndexKey(it->get_str(), hashBytes, type)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            }
            addresses.push_back(std::make_pair(hashBytes, type));
        }
    } else {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    return true;
}

bool heightSort(std::pair<CAddressUnspentKey, CAddressUnspentValue> a,
                std::pair<CAddressUnspentKey, CAddressUnspentValue> b) {
    return a.second.blockHeight < b.second.blockHeight;
}

bool timestampSort(std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta> a,
                   std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta> b) {
    return a.second.time < b.second.time;
}

bool getAddressFromIndex(const int &type, const uint256 &hash, std::string &address)
{
    if (type == 2) {
        std::vector<unsigned char> addressBytes(hash.begin(), hash.begin() + 20);
        address = EncodeDestination(ScriptHash(uint160(addressBytes)));
    } else if (type == 1) {
        std::vector<unsigned char> addressBytes(hash.begin(), hash.begin() + 20);
        address = EncodeDestination(PKHash(uint160(addressBytes)));
    } else if (type == 3) {
        address = EncodeDestination(WitnessV0ScriptHash(hash));
    } else if (type == 4) {
        std::vector<unsigned char> addressBytes(hash.begin(), hash.begin() + 20);
        address = EncodeDestination(WitnessV0KeyHash(uint160(addressBytes)));
    } else {
        return false;
    }
    return true;
}

RPCHelpMan getaddressdeltas()
{
    return RPCHelpMan{"getaddressdeltas",
            "\nReturns all changes for an address (requires addressindex to be enabled).\n",
            {
                {"argument", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Json object",
                    {
                        {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "The qtum addresses",
                            {
                                {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The qtum address"},
                            }
                        },
                        {"start", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The start block height"},
                        {"end", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The end block height"},
                        {"chainInfo", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Include chain info in results, only applies if start and end specified"},
                    }
                }
            },
            {
                RPCResult{"if chainInfo is set to false",
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::NUM, "satoshis", "The difference of satoshis"},
                            {RPCResult::Type::STR_HEX, "txid", "The related txid"},
                            {RPCResult::Type::NUM, "index", "The related input or output index"},
                            {RPCResult::Type::NUM, "blockindex", "The transaction index in block"},
                            {RPCResult::Type::NUM, "height", "The block height"},
                            {RPCResult::Type::STR, "address", "The qtum address"},
                        }}
                    },
                },
                RPCResult{"if chainInfo is set to true",
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::ARR, "deltas", "List of delta",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::NUM, "satoshis", "The difference of satoshis"},
                                {RPCResult::Type::STR_HEX, "txid", "The related txid"},
                                {RPCResult::Type::NUM, "index", "The related input or output index"},
                                {RPCResult::Type::NUM, "blockindex", "The transaction index in block"},
                                {RPCResult::Type::NUM, "height", "The block height"},
                                {RPCResult::Type::STR, "address", "The qtum address"},
                            }}
                        }},
                        {RPCResult::Type::OBJ, "start", "Start block",
                        {
                            {RPCResult::Type::STR_HEX, "hash", "The block hash"},
                            {RPCResult::Type::NUM, "height", "The block height"},
                        }},
                        {RPCResult::Type::OBJ, "end", "End block",
                        {
                            {RPCResult::Type::STR_HEX, "hash", "The block hash"},
                            {RPCResult::Type::NUM, "height", "The block height"},
                        }},
                    },
                },
            },
            RPCExamples{
                HelpExampleCli("getaddressdeltas", "'{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}'")
        + HelpExampleRpc("getaddressdeltas", "{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}") +
                HelpExampleCli("getaddressdeltas", "'{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"], \"start\": 5000, \"end\": 5500, \"chainInfo\": true}'")
        + HelpExampleRpc("getaddressdeltas", "{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"], \"start\": 5000, \"end\": 5500, \"chainInfo\": true}")
            },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    UniValue startValue = find_value(request.params[0].get_obj(), "start");
    UniValue endValue = find_value(request.params[0].get_obj(), "end");

    UniValue chainInfo = find_value(request.params[0].get_obj(), "chainInfo");
    bool includeChainInfo = false;
    if (chainInfo.isBool()) {
        includeChainInfo = chainInfo.get_bool();
    }

    int start = 0;
    int end = 0;

    if (startValue.isNum() && endValue.isNum()) {
        start = startValue.get_int();
        end = endValue.get_int();
        if (start <= 0 || end <= 0) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Start and end is expected to be greater than zero");
        }
        if (end < start) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "End value is expected to be greater than start");
        }
    }

    std::vector<std::pair<uint256, int> > addresses;

    if (!getAddressesFromParams(request.params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    std::vector<std::pair<CAddressIndexKey, CAmount> > addressIndex;

    for (std::vector<std::pair<uint256, int> >::iterator it = addresses.begin(); it != addresses.end(); it++) {
        if (start > 0 && end > 0) {
            if (!GetAddressIndex((*it).first, (*it).second, addressIndex, chainman.m_blockman, start, end)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
            }
        } else {
            if (!GetAddressIndex((*it).first, (*it).second, addressIndex, chainman.m_blockman)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
            }
        }
    }

    UniValue deltas(UniValue::VARR);

    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it=addressIndex.begin(); it!=addressIndex.end(); it++) {
        std::string address;
        if (!getAddressFromIndex(it->first.type, it->first.hashBytes, address)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");
        }

        UniValue delta(UniValue::VOBJ);
        delta.pushKV("satoshis", it->second);
        delta.pushKV("txid", it->first.txhash.GetHex());
        delta.pushKV("index", (int)it->first.index);
        delta.pushKV("blockindex", (int)it->first.txindex);
        delta.pushKV("height", it->first.blockHeight);
        delta.pushKV("address", address);
        deltas.push_back(delta);
    }

    UniValue result(UniValue::VOBJ);

    if (includeChainInfo && start > 0 && end > 0) {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);

        CChain& active_chain = chainman.ActiveChain();
        if (start > active_chain.Height() || end > active_chain.Height()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Start or end is outside chain range");
        }

        CBlockIndex* startIndex = active_chain[start];
        CBlockIndex* endIndex = active_chain[end];

        UniValue startInfo(UniValue::VOBJ);
        UniValue endInfo(UniValue::VOBJ);

        startInfo.pushKV("hash", startIndex->GetBlockHash().GetHex());
        startInfo.pushKV("height", start);

        endInfo.pushKV("hash", endIndex->GetBlockHash().GetHex());
        endInfo.pushKV("height", end);

        result.pushKV("deltas", deltas);
        result.pushKV("start", startInfo);
        result.pushKV("end", endInfo);

        return result;
    } else {
        return deltas;
    }
},
    };
}

RPCHelpMan getaddressbalance()
{
    return RPCHelpMan{"getaddressbalance",
                "\nReturns the balance for an address(es) (requires addressindex to be enabled).\n",
                {
                    {"argument", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Json object",
                        {
                            {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "The qtum addresses",
                                {
                                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The qtum address"},
                                }
                            },
                        }
                    }
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "balance", "The current balance in satoshis"},
                        {RPCResult::Type::NUM, "received", "The total number of satoshis received (including change)"},
                        {RPCResult::Type::NUM, "immature", "The immature balance in satoshis"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getaddressbalance", "'{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}'")
            + HelpExampleRpc("getaddressbalance", "{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    std::vector<std::pair<uint256, int> > addresses;

    if (!getAddressesFromParams(request.params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    std::vector<std::pair<CAddressIndexKey, CAmount> > addressIndex;

    for (std::vector<std::pair<uint256, int> >::iterator it = addresses.begin(); it != addresses.end(); it++) {
        if (!GetAddressIndex((*it).first, (*it).second, addressIndex, chainman.m_blockman)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
        }
    }

    CAmount balance = 0;
    CAmount received = 0;
    CAmount immature = 0;

    LOCK(cs_main);
    CChain& active_chain = chainman.ActiveChain();
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it=addressIndex.begin(); it!=addressIndex.end(); it++) {
        if (it->second > 0) {
            received += it->second;
        }
        balance += it->second;
        int nHeight = active_chain.Height();
        if (it->first.txindex == 1 && ((nHeight - it->first.blockHeight) < Params().GetConsensus().CoinbaseMaturity(nHeight)))
            immature += it->second; //immature stake outputs
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("balance", balance);
    result.pushKV("received", received);
    result.pushKV("immature", immature);

    return result;
},
    };
}

RPCHelpMan getaddressutxos()
{
    return RPCHelpMan{"getaddressutxos",
                "\nReturns all unspent outputs for an address (requires addressindex to be enabled).\n",
                {
                    {"argument", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Json object",
                        {
                            {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "The qtum addresses",
                                {
                                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The qtum address"},
                                }
                            },
                            {"chainInfo", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Include chain info with results"},
                        }
                    }
                },
                {
                    RPCResult{"if chainInfo is set to false",
                        RPCResult::Type::ARR, "", "",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "address", "The address base58check encoded"},
                                {RPCResult::Type::STR_HEX, "txid", "The output txid"},
                                {RPCResult::Type::NUM, "height", "The block height"},
                                {RPCResult::Type::NUM, "outputIndex", "The output index"},
                                {RPCResult::Type::STR_HEX, "script", "The script hex encoded"},
                                {RPCResult::Type::NUM, "satoshis", "The number of satoshis of the output"},
                                {RPCResult::Type::BOOL, "isStake", "Is coinstake output"},
                            }}
                        },
                    },
                    RPCResult{"if chainInfo is set to true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ARR, "utxos", "List of utxo",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                {
                                    {RPCResult::Type::STR, "address", "The address base58check encoded"},
                                    {RPCResult::Type::STR_HEX, "txid", "The output txid"},
                                    {RPCResult::Type::NUM, "height", "The block height"},
                                    {RPCResult::Type::NUM, "outputIndex", "The output index"},
                                    {RPCResult::Type::STR_HEX, "script", "The script hex encoded"},
                                    {RPCResult::Type::NUM, "satoshis", "The number of satoshis of the output"},
                                    {RPCResult::Type::BOOL, "isStake", "Is coinstake output"},
                                }}
                            }},
                            {RPCResult::Type::STR_HEX, "hash", "The tip block hash"},
                            {RPCResult::Type::NUM, "height", "The tip block height"},
                        },
                    },
                },
                RPCExamples{
                    HelpExampleCli("getaddressutxos", "'{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}'")
            + HelpExampleRpc("getaddressutxos", "{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}") +
                    HelpExampleCli("getaddressutxos", "'{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"], \"chainInfo\": true}'")
            + HelpExampleRpc("getaddressutxos", "{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"], \"chainInfo\": true}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    bool includeChainInfo = false;
    if (request.params[0].isObject()) {
        UniValue chainInfo = find_value(request.params[0].get_obj(), "chainInfo");
        if (chainInfo.isBool()) {
            includeChainInfo = chainInfo.get_bool();
        }
    }

    std::vector<std::pair<uint256, int> > addresses;

    if (!getAddressesFromParams(request.params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;

    for (std::vector<std::pair<uint256, int> >::iterator it = addresses.begin(); it != addresses.end(); it++) {
        if (!GetAddressUnspent((*it).first, (*it).second, unspentOutputs, chainman.m_blockman)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
        }
    }

    std::sort(unspentOutputs.begin(), unspentOutputs.end(), heightSort);

    UniValue utxos(UniValue::VARR);

    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it=unspentOutputs.begin(); it!=unspentOutputs.end(); it++) {
        UniValue output(UniValue::VOBJ);
        std::string address;
        if (!getAddressFromIndex(it->first.type, it->first.hashBytes, address)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");
        }

        output.pushKV("address", address);
        output.pushKV("txid", it->first.txhash.GetHex());
        output.pushKV("outputIndex", (int)it->first.index);
        output.pushKV("script", HexStr(MakeUCharSpan(it->second.script)));
        output.pushKV("satoshis", it->second.satoshis);
        output.pushKV("height", it->second.blockHeight);
        output.pushKV("isStake", it->second.coinStake);
        utxos.push_back(output);
    }

    if (includeChainInfo) {
        UniValue result(UniValue::VOBJ);
        result.pushKV("utxos", utxos);

        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        CChain& active_chain = chainman.ActiveChain();
        result.pushKV("hash", active_chain.Tip()->GetBlockHash().GetHex());
        result.pushKV("height", (int)active_chain.Height());
        return result;
    } else {
        return utxos;
    }
},
    };
}

RPCHelpMan getaddressmempool()
{
    return RPCHelpMan{"getaddressmempool",
                "\nReturns all mempool deltas for an address (requires addressindex to be enabled).\n",
                {
                    {"argument", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Json object",
                        {
                            {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "The qtum addresses",
                                {
                                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The qtum address"},
                                }
                            },
                        }
                    }
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR, "address", "The qtum address"},
                            {RPCResult::Type::STR_HEX, "txid", "The related txid"},
                            {RPCResult::Type::NUM, "index", "The related input or output index"},
                            {RPCResult::Type::NUM, "satoshis", "The difference of satoshis"},
                            {RPCResult::Type::NUM, "timestamp", "The time the transaction entered the mempool (seconds)"},
                            {RPCResult::Type::STR_HEX, "prevtxid", "The previous txid (if spending)"},
                            {RPCResult::Type::NUM, "prevout", "The previous transaction output index (if spending)"},
                        }}
                    }
                },
                RPCExamples{
                    HelpExampleCli("getaddressmempool", "'{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}'")
            + HelpExampleRpc("getaddressmempool", "{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    const NodeContext& node = EnsureAnyNodeContext(request.context);
    std::vector<std::pair<uint256, int> > addresses;

    if (!getAddressesFromParams(request.params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta> > indexes;

    if (!node.mempool->getAddressIndex(addresses, indexes)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
    }

    std::sort(indexes.begin(), indexes.end(), timestampSort);

    UniValue result(UniValue::VARR);

    for (std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta> >::iterator it = indexes.begin();
         it != indexes.end(); it++) {

        std::string address;
        if (!getAddressFromIndex(it->first.type, it->first.addressBytes, address)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");
        }

        UniValue delta(UniValue::VOBJ);
        delta.pushKV("address", address);
        delta.pushKV("txid", it->first.txhash.GetHex());
        delta.pushKV("index", (int)it->first.index);
        delta.pushKV("satoshis", it->second.amount);
        delta.pushKV("timestamp", it->second.time);
        if (it->second.amount < 0) {
            delta.pushKV("prevtxid", it->second.prevhash.GetHex());
            delta.pushKV("prevout", (int)it->second.prevout);
        }
        result.push_back(delta);
    }

    return result;
},
    };
}

RPCHelpMan getblockhashes()
{
    return RPCHelpMan{"getblockhashes",
                "\nReturns array of hashes of blocks within the timestamp range provided.\n",
                {
                    {"high", RPCArg::Type::NUM, RPCArg::Optional::NO, "The newer block timestamp"},
                    {"low", RPCArg::Type::NUM, RPCArg::Optional::NO, "The older block timestamp"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "An object with options",
                        {
                            {"noOrphans", RPCArg::Type::BOOL, RPCArg::Default{"false"}, "Will only include blocks on the main chain"},
                            {"logicalTimes", RPCArg::Type::BOOL, RPCArg::Default{"false"}, "Will include logical timestamps with hashes"},
                        },
                    },
                },
                {
                    RPCResult{"if logicalTimes is set to false",
                        RPCResult::Type::ARR, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "", "The block hash"}
                        },
                    },
                    RPCResult{"if logicalTimes is set to true",
                        RPCResult::Type::ARR, "", "",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "blockhash", "The block hash"},
                                {RPCResult::Type::NUM, "logicalts", "The logical timestamp"},
                            }}
                        },
                    },
                },
                RPCExamples{
                    HelpExampleCli("getblockhashes", "1231614698 1231024505")
                    + HelpExampleCli("getblockhashes", "1231614698 1231024505 '{\"noOrphans\":false, \"logicalTimes\":true}'")
            + HelpExampleRpc("getblockhashes", "1231614698, 1231024505")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    unsigned int high = request.params[0].get_int();
    unsigned int low = request.params[1].get_int();
    bool fActiveOnly = false;
    bool fLogicalTS = false;

    if (!request.params[2].isNull()) {
        if (request.params[2].isObject()) {
            UniValue noOrphans = find_value(request.params[2].get_obj(), "noOrphans");
            UniValue returnLogical = find_value(request.params[2].get_obj(), "logicalTimes");

            if (noOrphans.isBool())
                fActiveOnly = noOrphans.get_bool();

            if (returnLogical.isBool())
                fLogicalTS = returnLogical.get_bool();
        }
    }

    std::vector<std::pair<uint256, unsigned int> > blockHashes;
    bool found = false;

    found = GetTimestampIndex(high, low, fActiveOnly, blockHashes, chainman);

    if (!found) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for block hashes");
    }

    UniValue result(UniValue::VARR);

    for (std::vector<std::pair<uint256, unsigned int> >::const_iterator it=blockHashes.begin(); it!=blockHashes.end(); it++) {
        if (fLogicalTS) {
            UniValue item(UniValue::VOBJ);
            item.pushKV("blockhash", it->first.GetHex());
            item.pushKV("logicalts", (int)it->second);
            result.push_back(item);
        } else {
            result.push_back(it->first.GetHex());
        }
    }

    return result;
},
    };
}

RPCHelpMan getspentinfo()
{
    return RPCHelpMan{"getspentinfo",
                "\nReturns the txid and index where an output is spent.\n",
                {
                    {"argument", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Transaction data",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the txid"},
                            {"index", RPCArg::Type::NUM, RPCArg::Optional::NO, "The start block height"},
                        },
                    },
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                        {RPCResult::Type::NUM, "index", "The spending input index"},
                        {RPCResult::Type::NUM, "height", "The spending block height"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getspentinfo", "'{\"txid\": \"0437cd7f8525ceed2324359c2d0ba26006d92d856a9c20fa0241106ee5a597c9\", \"index\": 0}'")
            + HelpExampleRpc("getspentinfo", "{\"txid\": \"0437cd7f8525ceed2324359c2d0ba26006d92d856a9c20fa0241106ee5a597c9\", \"index\": 0}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const CTxMemPool& mempool = EnsureMemPool(node);
    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    UniValue txidValue = find_value(request.params[0].get_obj(), "txid");
    UniValue indexValue = find_value(request.params[0].get_obj(), "index");

    if (!txidValue.isStr() || !indexValue.isNum()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid txid or index");
    }

    uint256 txid = ParseHashV(txidValue, "txid");
    int outputIndex = indexValue.get_int();

    CSpentIndexKey key(txid, outputIndex);
    CSpentIndexValue value;

    if (!GetSpentIndex(key, value, mempool, chainman.m_blockman)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to get spent info");
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", value.txid.GetHex());
    obj.pushKV("index", (int)value.inputIndex);
    obj.pushKV("height", value.blockHeight);

    return obj;
},
    };
}

RPCHelpMan getaddresstxids()
{
    return RPCHelpMan{"getaddresstxids",
                "\nReturns the txids for an address(es) (requires addressindex to be enabled).\n",
                {
                    {"argument", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Json object",
                        {
                            {"addresses", RPCArg::Type::ARR, RPCArg::Optional::NO, "The qtum addresses",
                                {
                                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The qtum address"},
                                }
                            },
                            {"start", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "The start block height"},
                            {"end", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "The end block height"},
                        }
                    }
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "transactionid", "The transaction id"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getaddresstxids", "'{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}'")
            + HelpExampleRpc("getaddresstxids", "{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"]}") +
                    HelpExampleCli("getaddresstxids", "'{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"], \"start\": 5000, \"end\": 5500}'")
            + HelpExampleRpc("getaddresstxids", "{\"addresses\": [\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"], \"start\": 5000, \"end\": 5500}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    std::vector<std::pair<uint256, int> > addresses;

    if (!getAddressesFromParams(request.params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    int start = 0;
    int end = 0;
    if (request.params[0].isObject()) {
        UniValue startValue = find_value(request.params[0].get_obj(), "start");
        UniValue endValue = find_value(request.params[0].get_obj(), "end");
        if (startValue.isNum() && endValue.isNum()) {
            start = startValue.get_int();
            end = endValue.get_int();
        }
    }

    std::vector<std::pair<CAddressIndexKey, CAmount> > addressIndex;

    for (std::vector<std::pair<uint256, int> >::iterator it = addresses.begin(); it != addresses.end(); it++) {
        if (start > 0 && end > 0) {
            if (!GetAddressIndex((*it).first, (*it).second, addressIndex, chainman.m_blockman, start, end)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
            }
        } else {
            if (!GetAddressIndex((*it).first, (*it).second, addressIndex, chainman.m_blockman)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
            }
        }
    }

    std::set<std::pair<int, std::string> > txids;
    UniValue result(UniValue::VARR);

    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it=addressIndex.begin(); it!=addressIndex.end(); it++) {
        int height = it->first.blockHeight;
        std::string txid = it->first.txhash.GetHex();

        if (addresses.size() > 1) {
            txids.insert(std::make_pair(height, txid));
        } else {
            if (txids.insert(std::make_pair(height, txid)).second) {
                result.push_back(txid);
            }
        }
    }

    if (addresses.size() > 1) {
        for (std::set<std::pair<int, std::string> >::const_iterator it=txids.begin(); it!=txids.end(); it++) {
            result.push_back(it->second);
        }
    }

    return result;
},
    };
}

std::vector<std::string> getListArgsType()
{
    std::vector<std::string> ret = { "-rpcwallet",
                                     "-rpcauth",
                                     "-rpcwhitelist",
                                     "-rpcallowip",
                                     "-rpcbind",
                                     "-blockfilterindex",
                                     "-whitebind",
                                     "-bind",
                                     "-debug",
                                     "-debugexclude",
                                     "-stakingallowlist",
                                     "-stakingexcludelist",
                                     "-uacomment",
                                     "-onlynet",
                                     "-externalip",
                                     "-loadblock",
                                     "-addnode",
                                     "-whitelist",
                                     "-seednode",
                                     "-connect",
                                     "-deprecatedrpc",
                                     "-wallet" };
    return ret;
}

RPCHelpMan listconf()
{
    return RPCHelpMan{"listconf",
                "\nReturns the current options that qtumd was started with.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "param1", "Value for param1"},
                        {RPCResult::Type::STR, "param2", "Value for param2"},
                        {RPCResult::Type::STR, "param3", "Value for param3"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("listconf", "")
            + HelpExampleRpc("listconf", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    UniValue ret(UniValue::VOBJ);

    std::vector<std::string> paramListType = getListArgsType();
    for (const auto& arg : gArgs.getArgsList(paramListType)) {
        UniValue listValues(UniValue::VARR);
        for (const auto& value : arg.second) {
            std::optional<unsigned int> flags = gArgs.GetArgFlags('-' + arg.first);
            if (flags) {
                UniValue value_param = (*flags & gArgs.SENSITIVE) ? "****" : value;
                listValues.push_back(value_param);
            }
        }

        int size = listValues.size();
        if(size > 0)
        {
            ret.pushKV(arg.first, size == 1 ? listValues[0] : listValues);
        }
    }
    return ret;
},
    };
}

static RPCHelpMan getdescriptorinfo()
{
    const std::string EXAMPLE_DESCRIPTOR = "wpkh([d34db33f/84h/0h/0h]0279be667ef9dcbbac55a06295Ce870b07029Bfcdb2dce28d959f2815b16f81798)";

    return RPCHelpMan{"getdescriptorinfo",
            {"\nAnalyses a descriptor.\n"},
            {
                {"descriptor", RPCArg::Type::STR, RPCArg::Optional::NO, "The descriptor."},
            },
            RPCResult{
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR, "descriptor", "The descriptor in canonical form, without private keys"},
                    {RPCResult::Type::STR, "checksum", "The checksum for the input descriptor"},
                    {RPCResult::Type::BOOL, "isrange", "Whether the descriptor is ranged"},
                    {RPCResult::Type::BOOL, "issolvable", "Whether the descriptor is solvable"},
                    {RPCResult::Type::BOOL, "hasprivatekeys", "Whether the input descriptor contained at least one private key"},
                }
            },
            RPCExamples{
                "Analyse a descriptor\n" +
                HelpExampleCli("getdescriptorinfo", "\"" + EXAMPLE_DESCRIPTOR + "\"") +
                HelpExampleRpc("getdescriptorinfo", "\"" + EXAMPLE_DESCRIPTOR + "\"")
            },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    RPCTypeCheck(request.params, {UniValue::VSTR});

    FlatSigningProvider provider;
    std::string error;
    auto desc = Parse(request.params[0].get_str(), provider, error);
    if (!desc) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("descriptor", desc->ToString());
    result.pushKV("checksum", GetDescriptorChecksum(request.params[0].get_str()));
    result.pushKV("isrange", desc->IsRange());
    result.pushKV("issolvable", desc->IsSolvable());
    result.pushKV("hasprivatekeys", provider.keys.size() > 0);
    return result;
},
    };
}

static RPCHelpMan deriveaddresses()
{
    const std::string EXAMPLE_DESCRIPTOR = "wpkh([d34db33f/84h/0h/0h]xpub6DJ2dNUysrn5Vt36jH2KLBT2i1auw1tTSSomg8PhqNiUtx8QX2SvC9nrHu81fT41fvDUnhMjEzQgXnQjKEu3oaqMSzhSrHMxyyoEAmUHQbY/0/*)#cjjspncu";

    return RPCHelpMan{"deriveaddresses",
            {"\nDerives one or more addresses corresponding to an output descriptor.\n"
            "Examples of output descriptors are:\n"
            "    pkh(<pubkey>)                        P2PKH outputs for the given pubkey\n"
            "    wpkh(<pubkey>)                       Native segwit P2PKH outputs for the given pubkey\n"
            "    sh(multi(<n>,<pubkey>,<pubkey>,...)) P2SH-multisig outputs for the given threshold and pubkeys\n"
            "    raw(<hex script>)                    Outputs whose scriptPubKey equals the specified hex scripts\n"
            "\nIn the above, <pubkey> either refers to a fixed public key in hexadecimal notation, or to an xpub/xprv optionally followed by one\n"
            "or more path elements separated by \"/\", where \"h\" represents a hardened child key.\n"
            "For more information on output descriptors, see the documentation in the doc/descriptors.md file.\n"},
            {
                {"descriptor", RPCArg::Type::STR, RPCArg::Optional::NO, "The descriptor."},
                {"range", RPCArg::Type::RANGE, RPCArg::Optional::OMITTED_NAMED_ARG, "If a ranged descriptor is used, this specifies the end or the range (in [begin,end] notation) to derive."},
            },
            RPCResult{
                RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::STR, "address", "the derived addresses"},
                }
            },
            RPCExamples{
                "First three native segwit receive addresses\n" +
                HelpExampleCli("deriveaddresses", "\"" + EXAMPLE_DESCRIPTOR + "\" \"[0,2]\"") +
                HelpExampleRpc("deriveaddresses", "\"" + EXAMPLE_DESCRIPTOR + "\", \"[0,2]\"")
            },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    RPCTypeCheck(request.params, {UniValue::VSTR, UniValueType()}); // Range argument is checked later
    const std::string desc_str = request.params[0].get_str();

    int64_t range_begin = 0;
    int64_t range_end = 0;

    if (request.params.size() >= 2 && !request.params[1].isNull()) {
        std::tie(range_begin, range_end) = ParseDescriptorRange(request.params[1]);
    }

    FlatSigningProvider key_provider;
    std::string error;
    auto desc = Parse(desc_str, key_provider, error, /* require_checksum = */ true);
    if (!desc) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
    }

    if (!desc->IsRange() && request.params.size() > 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Range should not be specified for an un-ranged descriptor");
    }

    if (desc->IsRange() && request.params.size() == 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Range must be specified for a ranged descriptor");
    }

    UniValue addresses(UniValue::VARR);

    for (int64_t i = range_begin; i <= range_end; ++i) {
        FlatSigningProvider provider;
        std::vector<CScript> scripts;
        if (!desc->Expand(i, key_provider, scripts, provider)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Cannot derive script without private keys");
        }

        for (const CScript &script : scripts) {
            CTxDestination dest;
            if (!ExtractDestination(script, dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Descriptor does not have a corresponding address");
            }

            addresses.push_back(EncodeDestination(dest));
        }
    }

    // This should not be possible, but an assert seems overkill:
    if (addresses.empty()) {
        throw JSONRPCError(RPC_MISC_ERROR, "Unexpected empty result");
    }

    return addresses;
},
    };
}

static RPCHelpMan verifymessage()
{
    return RPCHelpMan{"verifymessage",
                "Verify a signed message.",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The qtum address to use for the signature."},
                    {"signature", RPCArg::Type::STR, RPCArg::Optional::NO, "The signature provided by the signer in base 64 encoding (see signmessage)."},
                    {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The message that was signed."},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "If the signature is verified or not."
                },
                RPCExamples{
            "\nUnlock the wallet for 30 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n"
            + HelpExampleCli("signmessage", "\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"my message\"") +
            "\nVerify the signature\n"
            + HelpExampleCli("verifymessage", "\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"signature\" \"my message\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("verifymessage", "\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"signature\", \"my message\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    LOCK(cs_main);

    std::string strAddress  = request.params[0].get_str();
    std::string strSign     = request.params[1].get_str();
    std::string strMessage  = request.params[2].get_str();

    switch (MessageVerify(strAddress, strSign, strMessage)) {
    case MessageVerificationResult::ERR_INVALID_ADDRESS:
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    case MessageVerificationResult::ERR_ADDRESS_NO_KEY:
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    case MessageVerificationResult::ERR_MALFORMED_SIGNATURE:
        throw JSONRPCError(RPC_TYPE_ERROR, "Malformed base64 encoding");
    case MessageVerificationResult::ERR_PUBKEY_NOT_RECOVERED:
    case MessageVerificationResult::ERR_NOT_SIGNED:
        return false;
    case MessageVerificationResult::OK:
        return true;
    }

    return false;
},
    };
}

static RPCHelpMan signmessagewithprivkey()
{
    return RPCHelpMan{"signmessagewithprivkey",
                "\nSign a message with the private key of an address\n",
                {
                    {"privkey", RPCArg::Type::STR, RPCArg::Optional::NO, "The private key to sign the message with."},
                    {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The message to create a signature of."},
                },
                RPCResult{
                    RPCResult::Type::STR, "signature", "The signature of the message encoded in base 64"
                },
                RPCExamples{
            "\nCreate the signature\n"
            + HelpExampleCli("signmessagewithprivkey", "\"privkey\" \"my message\"") +
            "\nVerify the signature\n"
            + HelpExampleCli("verifymessage", "\"QD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"signature\" \"my message\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("signmessagewithprivkey", "\"privkey\", \"my message\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string strPrivkey = request.params[0].get_str();
    std::string strMessage = request.params[1].get_str();

    CKey key = DecodeSecret(strPrivkey);
    if (!key.IsValid()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
    }

    std::string signature;

    if (!MessageSign(key, strMessage, signature)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");
    }

    return signature;
},
    };
}

static RPCHelpMan setmocktime()
{
    return RPCHelpMan{"setmocktime",
        "\nSet the local time to given timestamp (-regtest only)\n",
        {
            {"timestamp", RPCArg::Type::NUM, RPCArg::Optional::NO, UNIX_EPOCH_TIME + "\n"
             "Pass 0 to go back to using the system time."},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!Params().IsMockableChain()) {
        throw std::runtime_error("setmocktime is for regression testing (-regtest mode) only");
    }

    // For now, don't change mocktime if we're in the middle of validation, as
    // this could have an effect on mempool time-based eviction, as well as
    // IsCurrentForFeeEstimation() and IsInitialBlockDownload().
    // TODO: figure out the right way to synchronize around mocktime, and
    // ensure all call sites of GetTime() are accessing this safely.
    LOCK(cs_main);

    RPCTypeCheck(request.params, {UniValue::VNUM});
    const int64_t time{request.params[0].get_int64()};
    if (time < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Mocktime cannot be negative: %s.", time));
    }
    SetMockTime(time);
    auto node_context = util::AnyPtr<NodeContext>(request.context);
    if (node_context) {
        for (const auto& chain_client : node_context->chain_clients) {
            chain_client->setMockTime(time);
        }
    }

    return NullUniValue;
},
    };
}

#if defined(USE_SYSCALL_SANDBOX)
static RPCHelpMan invokedisallowedsyscall()
{
    return RPCHelpMan{
        "invokedisallowedsyscall",
        "\nInvoke a disallowed syscall to trigger a syscall sandbox violation. Used for testing purposes.\n",
        {},
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{
            HelpExampleCli("invokedisallowedsyscall", "") + HelpExampleRpc("invokedisallowedsyscall", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            if (!Params().IsTestChain()) {
                throw std::runtime_error("invokedisallowedsyscall is used for testing only.");
            }
            TestDisallowedSandboxCall();
            return NullUniValue;
        },
    };
}
#endif // USE_SYSCALL_SANDBOX

static RPCHelpMan mockscheduler()
{
    return RPCHelpMan{"mockscheduler",
        "\nBump the scheduler into the future (-regtest only)\n",
        {
            {"delta_time", RPCArg::Type::NUM, RPCArg::Optional::NO, "Number of seconds to forward the scheduler into the future." },
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!Params().IsMockableChain()) {
        throw std::runtime_error("mockscheduler is for regression testing (-regtest mode) only");
    }

    // check params are valid values
    RPCTypeCheck(request.params, {UniValue::VNUM});
    int64_t delta_seconds = request.params[0].get_int64();
    if (delta_seconds <= 0 || delta_seconds > 3600) {
        throw std::runtime_error("delta_time must be between 1 and 3600 seconds (1 hr)");
    }

    auto node_context = util::AnyPtr<NodeContext>(request.context);
    // protect against null pointer dereference
    CHECK_NONFATAL(node_context);
    CHECK_NONFATAL(node_context->scheduler);
    node_context->scheduler->MockForward(std::chrono::seconds(delta_seconds));

    return NullUniValue;
},
    };
}

static UniValue RPCLockedMemoryInfo()
{
    LockedPool::Stats stats = LockedPoolManager::Instance().stats();
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("used", uint64_t(stats.used));
    obj.pushKV("free", uint64_t(stats.free));
    obj.pushKV("total", uint64_t(stats.total));
    obj.pushKV("locked", uint64_t(stats.locked));
    obj.pushKV("chunks_used", uint64_t(stats.chunks_used));
    obj.pushKV("chunks_free", uint64_t(stats.chunks_free));
    return obj;
}

#ifdef HAVE_MALLOC_INFO
static std::string RPCMallocInfo()
{
    char *ptr = nullptr;
    size_t size = 0;
    FILE *f = open_memstream(&ptr, &size);
    if (f) {
        malloc_info(0, f);
        fclose(f);
        if (ptr) {
            std::string rv(ptr, size);
            free(ptr);
            return rv;
        }
    }
    return "";
}
#endif

static RPCHelpMan getmemoryinfo()
{
    /* Please, avoid using the word "pool" here in the RPC interface or help,
     * as users will undoubtedly confuse it with the other "memory pool"
     */
    return RPCHelpMan{"getmemoryinfo",
                "Returns an object containing information about memory usage.\n",
                {
                    {"mode", RPCArg::Type::STR, RPCArg::Default{"stats"}, "determines what kind of information is returned.\n"
            "  - \"stats\" returns general statistics about memory usage in the daemon.\n"
            "  - \"mallocinfo\" returns an XML string describing low-level heap state (only available if compiled with glibc 2.10+)."},
                },
                {
                    RPCResult{"mode \"stats\"",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::OBJ, "locked", "Information about locked memory manager",
                            {
                                {RPCResult::Type::NUM, "used", "Number of bytes used"},
                                {RPCResult::Type::NUM, "free", "Number of bytes available in current arenas"},
                                {RPCResult::Type::NUM, "total", "Total number of bytes managed"},
                                {RPCResult::Type::NUM, "locked", "Amount of bytes that succeeded locking. If this number is smaller than total, locking pages failed at some point and key data could be swapped to disk."},
                                {RPCResult::Type::NUM, "chunks_used", "Number allocated chunks"},
                                {RPCResult::Type::NUM, "chunks_free", "Number unused chunks"},
                            }},
                        }
                    },
                    RPCResult{"mode \"mallocinfo\"",
                        RPCResult::Type::STR, "", "\"<malloc version=\"1\">...\""
                    },
                },
                RPCExamples{
                    HelpExampleCli("getmemoryinfo", "")
            + HelpExampleRpc("getmemoryinfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::string mode = request.params[0].isNull() ? "stats" : request.params[0].get_str();
    if (mode == "stats") {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("locked", RPCLockedMemoryInfo());
        return obj;
    } else if (mode == "mallocinfo") {
#ifdef HAVE_MALLOC_INFO
        return RPCMallocInfo();
#else
        throw JSONRPCError(RPC_INVALID_PARAMETER, "mallocinfo mode not available");
#endif
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "unknown mode " + mode);
    }
},
    };
}

static void EnableOrDisableLogCategories(UniValue cats, bool enable) {
    cats = cats.get_array();
    for (unsigned int i = 0; i < cats.size(); ++i) {
        std::string cat = cats[i].get_str();

        bool success;
        if (enable) {
            success = LogInstance().EnableCategory(cat);
        } else {
            success = LogInstance().DisableCategory(cat);
        }

        if (!success) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "unknown logging category " + cat);
        }
    }
}

static RPCHelpMan logging()
{
    return RPCHelpMan{"logging",
            "Gets and sets the logging configuration.\n"
            "When called without an argument, returns the list of categories with status that are currently being debug logged or not.\n"
            "When called with arguments, adds or removes categories from debug logging and return the lists above.\n"
            "The arguments are evaluated in order \"include\", \"exclude\".\n"
            "If an item is both included and excluded, it will thus end up being excluded.\n"
            "The valid logging categories are: " + LogInstance().LogCategoriesString() + "\n"
            "In addition, the following are available as category names with special meanings:\n"
            "  - \"all\",  \"1\" : represent all logging categories.\n"
            "  - \"none\", \"0\" : even if other logging categories are specified, ignore all of them.\n"
            ,
                {
                    {"include", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "The categories to add to debug logging",
                        {
                            {"include_category", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "the valid logging category"},
                        }},
                    {"exclude", RPCArg::Type::ARR, RPCArg::Optional::OMITTED_NAMED_ARG, "The categories to remove from debug logging",
                        {
                            {"exclude_category", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "the valid logging category"},
                        }},
                },
                RPCResult{
                    RPCResult::Type::OBJ_DYN, "", "keys are the logging categories, and values indicates its status",
                    {
                        {RPCResult::Type::BOOL, "category", "if being debug logged or not. false:inactive, true:active"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("logging", "\"[\\\"all\\\"]\" \"[\\\"http\\\"]\"")
            + HelpExampleRpc("logging", "[\"all\"], [\"libevent\"]")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint32_t original_log_categories = LogInstance().GetCategoryMask();
    if (request.params[0].isArray()) {
        EnableOrDisableLogCategories(request.params[0], true);
    }
    if (request.params[1].isArray()) {
        EnableOrDisableLogCategories(request.params[1], false);
    }
    uint32_t updated_log_categories = LogInstance().GetCategoryMask();
    uint32_t changed_log_categories = original_log_categories ^ updated_log_categories;

    // Update libevent logging if BCLog::LIBEVENT has changed.
    // If the library version doesn't allow it, UpdateHTTPServerLogging() returns false,
    // in which case we should clear the BCLog::LIBEVENT flag.
    // Throw an error if the user has explicitly asked to change only the libevent
    // flag and it failed.
    if (changed_log_categories & BCLog::LIBEVENT) {
        if (!UpdateHTTPServerLogging(LogInstance().WillLogCategory(BCLog::LIBEVENT))) {
            LogInstance().DisableCategory(BCLog::LIBEVENT);
            if (changed_log_categories == BCLog::LIBEVENT) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "libevent logging cannot be updated when using libevent before v2.1.1.");
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    for (const auto& logCatActive : LogInstance().LogCategoriesList()) {
        result.pushKV(logCatActive.category, logCatActive.active);
    }

    return result;
},
    };
}

static RPCHelpMan echo(const std::string& name)
{
    return RPCHelpMan{name,
                "\nSimply echo back the input arguments. This command is for testing.\n"
                "\nIt will return an internal bug report when arg9='trigger_internal_bug' is passed.\n"
                "\nThe difference between echo and echojson is that echojson has argument conversion enabled in the client-side table in "
                "qtum-cli and the GUI. There is no server-side difference.",
                {
                    {"arg0", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg1", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg2", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg3", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg4", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg5", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg6", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg7", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg8", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                    {"arg9", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, ""},
                },
                RPCResult{RPCResult::Type::ANY, "", "Returns whatever was passed in"},
                RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (request.params[9].isStr()) {
        CHECK_NONFATAL(request.params[9].get_str() != "trigger_internal_bug");
    }

    return request.params;
},
    };
}

static RPCHelpMan echo() { return echo("echo"); }
static RPCHelpMan echojson() { return echo("echojson"); }

static RPCHelpMan echoipc()
{
    return RPCHelpMan{
        "echoipc",
        "\nEcho back the input argument, passing it through a spawned process in a multiprocess build.\n"
        "This command is for testing.\n",
        {{"arg", RPCArg::Type::STR, RPCArg::Optional::NO, "The string to echo",}},
        RPCResult{RPCResult::Type::STR, "echo", "The echoed string."},
        RPCExamples{HelpExampleCli("echo", "\"Hello world\"") +
                    HelpExampleRpc("echo", "\"Hello world\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            interfaces::Init& local_init = *EnsureAnyNodeContext(request.context).init;
            std::unique_ptr<interfaces::Echo> echo;
            if (interfaces::Ipc* ipc = local_init.ipc()) {
                // Spawn a new bitcoin-node process and call makeEcho to get a
                // client pointer to a interfaces::Echo instance running in
                // that process. This is just for testing. A slightly more
                // realistic test spawning a different executable instead of
                // the same executable would add a new bitcoin-echo executable,
                // and spawn bitcoin-echo below instead of bitcoin-node. But
                // using bitcoin-node avoids the need to build and install a
                // new executable just for this one test.
                auto init = ipc->spawnProcess("bitcoin-node");
                echo = init->makeEcho();
                ipc->addCleanup(*echo, [init = init.release()] { delete init; });
            } else {
                // IPC support is not available because this is a bitcoind
                // process not a bitcoind-node process, so just create a local
                // interfaces::Echo object and return it so the `echoipc` RPC
                // method will work, and the python test calling `echoipc`
                // can expect the same result.
                echo = local_init.makeEcho();
            }
            return echo->echo(request.params[0].get_str());
        },
    };
}

static UniValue SummaryToJSON(const IndexSummary&& summary, std::string index_name)
{
    UniValue ret_summary(UniValue::VOBJ);
    if (!index_name.empty() && index_name != summary.name) return ret_summary;

    UniValue entry(UniValue::VOBJ);
    entry.pushKV("synced", summary.synced);
    entry.pushKV("best_block_height", summary.best_block_height);
    ret_summary.pushKV(summary.name, entry);
    return ret_summary;
}

static RPCHelpMan getindexinfo()
{
    return RPCHelpMan{"getindexinfo",
                "\nReturns the status of one or all available indices currently running in the node.\n",
                {
                    {"index_name", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Filter results for an index with a specific name."},
                },
                RPCResult{
                    RPCResult::Type::OBJ_DYN, "", "", {
                        {
                            RPCResult::Type::OBJ, "name", "The name of the index",
                            {
                                {RPCResult::Type::BOOL, "synced", "Whether the index is synced or not"},
                                {RPCResult::Type::NUM, "best_block_height", "The block height to which the index is synced"},
                            }
                        },
                    },
                },
                RPCExamples{
                    HelpExampleCli("getindexinfo", "")
                  + HelpExampleRpc("getindexinfo", "")
                  + HelpExampleCli("getindexinfo", "txindex")
                  + HelpExampleRpc("getindexinfo", "txindex")
                },
                [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue result(UniValue::VOBJ);
    const std::string index_name = request.params[0].isNull() ? "" : request.params[0].get_str();

    if (g_txindex) {
        result.pushKVs(SummaryToJSON(g_txindex->GetSummary(), index_name));
    }

    if (g_coin_stats_index) {
        result.pushKVs(SummaryToJSON(g_coin_stats_index->GetSummary(), index_name));
    }

    ForEachBlockFilterIndex([&result, &index_name](const BlockFilterIndex& index) {
        result.pushKVs(SummaryToJSON(index.GetSummary(), index_name));
    });

    return result;
},
    };
}

void RegisterMiscRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "control",            &getmemoryinfo,           },
    { "control",            &logging,                 },
    { "util",               &validateaddress,         },
    { "util",               &deriveaddresses,         },
    { "util",               &getdescriptorinfo,       },
    { "util",               &verifymessage,           },
    { "util",               &signmessagewithprivkey,  },
    { "util",               &getindexinfo,            },

    /* Not shown in help */
    { "hidden",             &setmocktime,             },
    { "hidden",             &mockscheduler,           },
    { "hidden",             &echo,                    },
    { "hidden",             &echojson,                },
    { "hidden",             &echoipc,                 },
    { "quagba",		    &mnauth		      },
    { "quagba",             &mnsync		      },
    { "quagba",		    &spork	              },
#if defined(USE_SYSCALL_SANDBOX)
    { "hidden",             &invokedisallowedsyscall, },
#endif // USE_SYSCALL_SANDBOX

  /////////////////////////////////////////////////////////////////////////////////////////////////////////////// // qtum
    { "control",            &getdgpinfo,              },
    { "util",               &getaddresstxids,         },
    { "util",               &getaddressdeltas,        },
    { "util",               &getaddressbalance,       },
    { "util",               &getaddressutxos,         },
    { "util",               &getaddressmempool,       },
    { "util",               &getblockhashes,          },
    { "util",               &getspentinfo,            },
    { "util",               &listconf,                },
  ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
