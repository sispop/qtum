// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/masternodemeta.h>

#include <timedata.h>
#include <util/string.h>

CMasternodeMetaMan mmetaman;

const std::string CMasternodeMetaMan::SERIALIZATION_VERSION_STRING = "CMasternodeMetaMan-Version-3";

UniValue CMasternodeMetaInfo::ToJson() const
{
    UniValue ret(UniValue::VOBJ);

    auto now = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    ret.pushKV("outboundAttemptCount", outboundAttemptCount.load());
    ret.pushKV("lastOutboundAttempt", lastOutboundAttempt.load());
    ret.pushKV("lastOutboundAttemptElapsed", now - lastOutboundAttempt);
    ret.pushKV("lastOutboundSuccess", lastOutboundSuccess.load());
    ret.pushKV("lastOutboundSuccessElapsed", now - lastOutboundSuccess);

    return ret;
}

void CMasternodeMetaInfo::AddGovernanceVote(const uint256& nGovernanceObjectHash)
{
    LOCK(cs);
    // Insert a zero value, or not. Then increment the value regardless. This
    // ensures the value is in the map.
    const auto& pair = mapGovernanceObjectsVotedOn.emplace(nGovernanceObjectHash, 0);
    pair.first->second++;
}

void CMasternodeMetaInfo::RemoveGovernanceObject(const uint256& nGovernanceObjectHash)
{
    LOCK(cs);
    // Whether or not the govobj hash exists in the map first is irrelevant.
    mapGovernanceObjectsVotedOn.erase(nGovernanceObjectHash);
}

CMasternodeMetaInfoPtr CMasternodeMetaMan::GetMetaInfo(const uint256& proTxHash, bool fCreate)
{
    LOCK(cs);
    auto it = metaInfos.find(proTxHash);
    if (it != metaInfos.end()) {
        return it->second;
    }
    if (!fCreate) {
        return nullptr;
    }
    it = metaInfos.emplace(proTxHash, std::make_shared<CMasternodeMetaInfo>(proTxHash)).first;
    return it->second;
}

bool CMasternodeMetaMan::AddGovernanceVote(const uint256& proTxHash, const uint256& nGovernanceObjectHash)
{
    auto mm = GetMetaInfo(proTxHash);
    mm->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CMasternodeMetaMan::RemoveGovernanceObject(const uint256& nGovernanceObjectHash)
{
    LOCK(cs);
    for(const auto& p : metaInfos) {
        p.second->RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

std::vector<uint256> CMasternodeMetaMan::GetAndClearDirtyGovernanceObjectHashes()
{
    LOCK(cs);
    std::vector<uint256> vecTmp = std::move(vecDirtyGovernanceObjectHashes);
    vecDirtyGovernanceObjectHashes.clear();
    return vecTmp;
}

void CMasternodeMetaMan::Clear()
{
    LOCK(cs);
    metaInfos.clear();
    vecDirtyGovernanceObjectHashes.clear();
}

std::string CMasternodeMetaMan::ToString() const
{
    std::ostringstream info;
    LOCK(cs);
    info << "Masternodes: meta infos object count: " << (int)metaInfos.size();
    return info.str();
}
