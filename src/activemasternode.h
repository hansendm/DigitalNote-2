// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The DarkCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef ACTIVEMASTERNODE_H
#define ACTIVEMASTERNODE_H

#include "uint/uint256.h"
#include "sync.h"
#include "masternode.h"
#include "init.h"
#include "mnengine.h"

class COutput;
class CKey;

// Responsible for activating the masternode and pinging the network
class CActiveMasternode
{
public:
	// Initialized by init.cpp
	// Keys for the main masternode
	CPubKey pubKeyMasternode;

	// Initialized while registering masternode
	CTxIn vin;
    CService service;

    int status;
    std::string notCapableReason;

    CActiveMasternode()
    {        
        status = MASTERNODE_NOT_PROCESSED;
    }

    void ManageStatus(); // manage status of main masternode

    bool Dseep(std::string& errorMessage); // ping for main masternode
    bool Dseep(CTxIn vin, CService service, CKey key, CPubKey pubKey, std::string &retErrorMessage, bool stop); // ping for any masternode

    bool StopMasterNode(std::string& errorMessage); // stop main masternode
    bool StopMasterNode(const std::string &strService, const std::string &strKeyMasternode, std::string& errorMessage); // stop remote masternode
    bool StopMasterNode(CTxIn vin, CService service, CKey key, CPubKey pubKey, std::string& errorMessage); // stop any masternode

    /// Register remote Masternode
    bool Register(const std::string &strService, const std::string &strKey, const std::string &txHash, const std::string &strOutputIndex, const std::string &strDonationAddress, const std::string &strDonationPercentage, std::string& errorMessage); 
    /// Register any Masternode
    bool Register(CTxIn vin, CService service, CKey key, CPubKey pubKey, CKey keyMasternode, CPubKey pubKeyMasternode, CScript donationAddress, int donationPercentage, std::string &retErrorMessage);  

    // get 2,000,000 XDN input that can be used for the masternode
    bool GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey);
    bool GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey, std::string strTxHash, const std::string &strOutputIndex);
    bool GetMasterNodeVinForPubKey(const std::string &collateralAddress, CTxIn& vin, CPubKey& pubkey, CKey& secretKey);
    bool GetMasterNodeVinForPubKey(const std::string &collateralAddress, CTxIn& vin, CPubKey& pubkey, CKey& secretKey, const std::string &strTxHash, const std::string &strOutputIndex);
    std::vector<COutput> SelectCoinsMasternode();
    std::vector<COutput> SelectCoinsMasternodeForPubKey(const std::string &collateralAddress);
    bool GetVinFromOutput(COutput out, CTxIn& vin, CPubKey& pubkey, CKey& secretKey);

    // enable hot wallet mode (run a masternode with no funds)
    bool EnableHotColdMasterNode(CTxIn& vin, CService& addr);
};

#endif
