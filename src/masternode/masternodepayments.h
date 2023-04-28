// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SYSCOIN_MASTERNODE_MASTERNODEPAYMENTS_H
#define SYSCOIN_MASTERNODE_MASTERNODEPAYMENTS_H



class CMasternodePayments;
class CChain;
/// TODO: all 4 functions do not belong here really, they should be refactored/moved somewhere (main.cpp ?)
bool IsBlockValueValid(const CBlock& block, int nBlockHeight, const CAmount &blockReward, std::string& strErrorRet);
bool IsBlockPayeeValid(CChain& activeChain, const CTransaction& txNew, int nBlockHeight, const CAmount &blockReward, const CAmount &fees, CAmount& nMNSeniorityRet, CAmount &nMNFloorDiffRet);
void FillBlockPayments(CChain& activeChain, CMutableTransaction& txNew, int nBlockHeight, const CAmount &blockReward, const CAmount &fees, std::vector<CTxOut>& voutMasternodePaymentsRet, std::vector<CTxOut>& voutSuperblockPaymentsRet);

extern CMasternodePayments mnpayments;

//
// Masternode Payments Class
// Keeps track of who should get paid for which blocks
//

class CMasternodePayments
{
public:
    static bool GetBlockTxOuts(CChain& activeChain, int nBlockHeight, const CAmount &blockReward, std::vector<CTxOut>& voutMasternodePaymentsRet, const CAmount &nHalfFee, CAmount& nMNSeniorityRet, CAmount &nMNFloorDiffRet, int& nCollateralHeight);
    static bool IsTransactionValid(CChain& activeChain, const CTransaction& txNew, int nBlockHeight, const CAmount &blockReward, const CAmount& nHalfFee, CAmount& nMNSeniorityRet, CAmount &nMNFloorDiffRet);
    static bool GetMasternodeTxOuts(CChain& activeChain, int nBlockHeight, const CAmount &blockReward, std::vector<CTxOut>& voutMasternodePaymentsRet, const CAmount &nHalfFee);
};

#endif // SYSCOIN_MASTERNODE_MASTERNODEPAYMENTS_H
