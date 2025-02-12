#include "compat.h"

#include <memory>
#include <openssl/sha.h>
#include <boost/tuple/tuple.hpp>

#include "blockparams.h"
#include "txdb-leveldb.h"
#include "kernel.h"
#include "cmasternode.h"
#include "cmasternodeman.h"
#include "cmasternodepayments.h"
#include "masternodeman.h"
#include "masternode_extern.h"
#include "fork.h"
#include "cblock.h"
#include "creservekey.h"
#include "cwallet.h"
#include "script.h"
#include "net.h"
#include "main_const.h"
#include "ctxmempool.h"
#include "ctxout.h"
#include "ctransaction.h"
#include "main_extern.h"
#include "cbitcoinaddress.h"
#include "chainparams.h"
#include "cchainparams.h"
#include "cnodestination.h"
#include "ckeyid.h"
#include "cscriptid.h"
#include "cstealthaddress.h"
#include "thread.h"
#include "util.h"
#include "cblockindex.h"
#include "ctxindex.h"
#include "enums/serialize_type.h"
#include "serialize.h"

#include "miner.h"

//////////////////////////////////////////////////////////////////////////////
//
// DigitalNoteMiner
//

extern unsigned int nMinerSleep;

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
	unsigned char* pdata = (unsigned char*)pbuffer;
	unsigned int blocks = 1 + ((len + 8) / 64);
	unsigned char* pend = pdata + 64 * blocks;

	memset(pdata + len, 0, 64 * blocks - len);

	pdata[len] = 0x80;
	
	unsigned int bits = len * 8;
	
	pend[-1] = (bits >> 0) & 0xff;
	pend[-2] = (bits >> 8) & 0xff;
	pend[-3] = (bits >> 16) & 0xff;
	pend[-4] = (bits >> 24) & 0xff;
	
	return blocks;
}

static const unsigned int pSHA256InitState[8] =
{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
	SHA256_CTX ctx;
	unsigned char data[64];

	SHA256_Init(&ctx);

	for (int i = 0; i < 16; i++)
	{
		((uint32_t*)data)[i] = ByteReverse(((uint32_t*)pinput)[i]);
	}

	for (int i = 0; i < 8; i++)
	{
		ctx.h[i] = ((uint32_t*)pinit)[i];
	}

	SHA256_Update(&ctx, data, sizeof(data));

	for (int i = 0; i < 8; i++)
	{
		((uint32_t*)pstate)[i] = ctx.h[i];
	}
}

// Some explaining would be appreciated
class COrphan
{
public:
	CTransaction* ptx;
	std::set<uint256> setDependsOn;
	double dPriority;
	double dFeePerKb;

	COrphan(CTransaction* ptxIn)
	{
		ptx = ptxIn;
		dPriority = dFeePerKb = 0;
	}
};


uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
int64_t nLastCoinStakeSearchInterval = 0;
 
// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, CTransaction*> TxPriority;

class TxPriorityCompare
{
	bool byFee;

public:
	TxPriorityCompare(bool _byFee) : byFee(_byFee) { }
	
	bool operator()(const TxPriority& a, const TxPriority& b)
	{
		if (byFee)
		{
			if (a.get<1>() == b.get<1>())
			{
				return a.get<0>() < b.get<0>();
			}
			
			return a.get<1>() < b.get<1>();
		}
		else
		{
			if (a.get<0>() == b.get<0>())
			{
				return a.get<1>() < b.get<1>();
			}
			
			return a.get<0>() < b.get<0>();
		}
	}
};

// CreateNewBlock: create new block (without proof-of-work/proof-of-stake)
CBlock* CreateNewBlock(CReserveKey& reservekey, bool fProofOfStake, int64_t* pFees)
{
	// Create new block
	CBlockPtr pblock(new CBlock());

	if (!pblock.get())
	{
		return NULL;
	}

	CBlockIndex* pindexPrev = pindexBest;
	int nHeight = pindexPrev->nHeight + 1;

	// Create coinbase tx
	CTransaction txNew;
	txNew.vin.resize(1);
	txNew.vin[0].prevout.SetNull();
	//txNew.vin[0].scriptSig = CScript() << nHeight;
	txNew.vout.resize(1);

	if (!fProofOfStake)
	{
		CPubKey pubkey;
		
		if (!reservekey.GetReservedKey(pubkey))
		{
			return NULL;
		}
		
		txNew.vout[0].scriptPubKey.SetDestination(pubkey.GetID());
	}	
	else
	{
		// Height first in coinbase required for block.version=2
		txNew.vin[0].scriptSig = (CScript() << nHeight) + COINBASE_FLAGS;
		assert(txNew.vin[0].scriptSig.size() <= 100);

		txNew.vout[0].SetEmpty();
	}

	// Add our coinbase tx as first transaction
	pblock->vtx.push_back(txNew);

	// Largest block you're willing to create:
	unsigned int nBlockMaxSize = GetArg("-blockmaxsize", MAX_BLOCK_SIZE_GEN/2);
	// Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
	nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

	// How much of the block should be dedicated to high-priority transactions,
	// included regardless of the fees they pay
	unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
	nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

	// Minimum block size you want to create; block will be filled with free transactions
	// until there are no more or the block reaches this size:
	unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
	nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

	// Fee-per-kilobyte amount considered the same as "free"
	// Be careful setting this: if you set it to zero then
	// a transaction spammer can cheaply fill blocks using
	// 1-satoshi-fee transactions. It should be set above the real
	// cost to you of processing a transaction.
	int64_t nMinTxFee = MIN_TX_FEE;
	if (mapArgs.count("-mintxfee"))
	{
		ParseMoney(mapArgs["-mintxfee"], nMinTxFee);
	}

	pblock->nBits = GetNextTargetRequired(pindexPrev, fProofOfStake);

	// Collect memory pool transactions into the block
	int64_t nFees = 0;

	{
		LOCK2(cs_main, mempool.cs);
		
		CTxDB txdb("r");
	//> XDN <
		// Priority order to process transactions
		std::list<COrphan> vOrphan; // list memory doesn't move
		std::map<uint256, std::vector<COrphan*> > mapDependers;

		// This vector will be sorted into a priority queue:
		std::vector<TxPriority> vecPriority;
		vecPriority.reserve(mempool.mapTx.size());
		
		for (std::map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
		{
			CTransaction& tx = (*mi).second;
			
			if (tx.IsCoinBase() || tx.IsCoinStake() || !IsFinalTx(tx, nHeight))
			{
				continue;
			}
			
			COrphan* porphan = NULL;
			double dPriority = 0;
			int64_t nTotalIn = 0;
			bool fMissingInputs = false;
			
			for(const CTxIn& txin : tx.vin)
			{
				// Read prev transaction
				CTransaction txPrev;
				CTxIndex txindex;
				
				if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
				{
					#ifdef ENABLE_ORPHAN_TRANSACTIONS
						// This should never happen; all transactions in the memory
						// pool should connect to either transactions in the chain
						// or other transactions in the memory pool.
						if (!mempool.mapTx.count(txin.prevout.hash))
						{
							LogPrintf("ERROR: mempool transaction missing input\n");
							
							if (fDebug)
							{
								assert("mempool transaction missing input" == 0);
							}
							
							fMissingInputs = true;
							
							if (porphan)
							{
								vOrphan.pop_back();
							}
							
							break;
						}

						// Has to wait for dependencies
						if (!porphan)
						{
							// Use list for automatic deletion
							vOrphan.push_back(COrphan(&tx));
							porphan = &vOrphan.back();
						}
						
						mapDependers[txin.prevout.hash].push_back(porphan);
						porphan->setDependsOn.insert(txin.prevout.hash);
						nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
					#else // ENABLE_ORPHAN_TRANSACTIONS
						fMissingInputs = true;
					#endif // ENABLE_ORPHAN_TRANSACTIONS
					
					continue;
				}
				
				int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
				nTotalIn += nValueIn;

				int nConf = txindex.GetDepthInMainChain();
				dPriority += (double)nValueIn * nConf;
			}
			
			if (fMissingInputs)
			{
				continue;
			}
			
			// Priority is sum(valuein * age) / txsize
			unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
			dPriority /= nTxSize;

			// This is a more accurate fee-per-kilobyte than is used by the client code, because the
			// client code rounds up the size to the nearest 1K. That's good, because it gives an
			// incentive to create smaller transactions.
			double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize)/1000.0);
			
			#ifdef ENABLE_ORPHAN_TRANSACTIONS
				if (porphan)
				{
					porphan->dPriority = dPriority;
					porphan->dFeePerKb = dFeePerKb;
				}
				else
				{
					vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
				}
			#else // ENABLE_ORPHAN_TRANSACTIONS
				vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
			#endif // ENABLE_ORPHAN_TRANSACTIONS
		}

		// Collect transactions into block
		std::map<uint256, CTxIndex> mapTestPool;
		uint64_t nBlockSize = 1000;
		uint64_t nBlockTx = 0;
		int nBlockSigOps = 100;
		bool fSortedByFee = (nBlockPrioritySize <= 0);

		TxPriorityCompare comparer(fSortedByFee);
		std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

		while (!vecPriority.empty())
		{
			// Take highest priority transaction off the priority queue:
			double dPriority = vecPriority.front().get<0>();
			double dFeePerKb = vecPriority.front().get<1>();
			CTransaction& tx = *(vecPriority.front().get<2>());

			std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
			vecPriority.pop_back();

			// Size limits
			unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
			if (nBlockSize + nTxSize >= nBlockMaxSize)
			{
				continue;
			}
			
			// Legacy limits on sigOps:
			unsigned int nTxSigOps = GetLegacySigOpCount(tx);
			if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
			{
				continue;
			}
			
			// Timestamp limit
			if (tx.nTime > GetAdjustedTime() || (fProofOfStake && tx.nTime > pblock->vtx[0].nTime))
			{
				continue;
			}
			
			// Skip free transactions if we're past the minimum block size:
			if (fSortedByFee && (dFeePerKb < nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
			{
				continue;
			}
			
			// Prioritize by fee once past the priority size or we run out of high-priority
			// transactions:
			if (!fSortedByFee &&
				((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
			{
				fSortedByFee = true;
				comparer = TxPriorityCompare(fSortedByFee);
				std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
			}

			// Connecting shouldn't fail due to dependency on other memory pool transactions
			// because we're already processing them in order of dependency
			std::map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
			mapPrevTx_t mapInputs;
			bool fInvalid;
			
			if (!tx.FetchInputs(txdb, mapTestPoolTmp, false, true, mapInputs, fInvalid))
			{
				continue;
			}
			
			int64_t nTxFees = tx.GetValueMapIn(mapInputs)-tx.GetValueOut();

			nTxSigOps += GetP2SHSigOpCount(tx, mapInputs);
			
			if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
			{
				continue;
			}
			
			// Note that flags: we don't want to set mempool/IsStandard()
			// policy here, but we still have to ensure that the block we
			// create only contains transactions that are valid in new blocks.
			if (!tx.ConnectInputs(txdb, mapInputs, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, false, true, MANDATORY_SCRIPT_VERIFY_FLAGS))
			{
				continue;
			}
			
			mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(1,1,1), tx.vout.size());
			swap(mapTestPool, mapTestPoolTmp);

			// Added
			pblock->vtx.push_back(tx);
			nBlockSize += nTxSize;
			++nBlockTx;
			nBlockSigOps += nTxSigOps;
			nFees += nTxFees;

			if (fDebug && GetBoolArg("-printpriority", false))
			{
				LogPrintf(
					"priority %.1f feeperkb %.1f txid %s\n",
					dPriority,
					dFeePerKb,
					tx.GetHash().ToString()
				);
			}

			// Add transactions that depend on this one to the priority queue
			uint256 hash = tx.GetHash();
			if (mapDependers.count(hash))
			{
				for(COrphan* porphan : mapDependers[hash])
				{
					if (!porphan->setDependsOn.empty())
					{
						porphan->setDependsOn.erase(hash);
						
						if (porphan->setDependsOn.empty())
						{
							vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
							
							std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
						}
					}
				}
			}
		}

		nLastBlockTx = nBlockTx;
		nLastBlockSize = nBlockSize;

		if (fDebug && GetBoolArg("-printpriority", false))
		{
			LogPrintf("CreateNewBlock(): total size %u\n", nBlockSize);
		}
		
		// > XDN <
		if (!fProofOfStake)
		{
			pblock->vtx[0].vout[0].nValue = GetProofOfWorkReward(pindexPrev->nHeight + 1, nFees);
			
			int64_t block_time = pindexBest->GetBlockTime();
			
			// Check for payment update fork
			if(block_time > 0)
			{
				if(block_time > VERION_1_0_1_5_MANDATORY_UPDATE_START) // Monday, May 20, 2019 12:00:00 AM
				{
					// masternode/devops payment
					int64_t blockReward = GetProofOfWorkReward(pindexPrev->nHeight + 1, nFees);
					bool hasPayment = true;
					bool bMasterNodePayment = true;// TODO: Setup proper network toggle
					CScript mn_payee;
					CScript do_payee;
					CTxIn vin;

					// Determine our payment address for devops
					//
					// OLD IMPLEMENTATION COMMNETED OUT
					// CScript devopsScript;
					// devopsScript << OP_DUP << OP_HASH160 << ParseHex(Params().DevOpsPubKey()) << OP_EQUALVERIFY << OP_CHECKSIG;
					// do_payee = devopsScript;
					//
					// Define Address
					//
					// TODO: Clean this up, it's a mess (could be done much more cleanly)
					//       Not an issue otherwise, merely a pet peev. Done in a rush...
					//
					CBitcoinAddress devopaddress;
					if (Params().NetworkID() == CChainParams_Network::MAIN)
					{
						devopaddress = CBitcoinAddress(getDevelopersAdress(pindexBest));
					}
					else if (Params().NetworkID() == CChainParams_Network::TESTNET)
					{
						devopaddress = CBitcoinAddress("");
					}
					else if (Params().NetworkID() == CChainParams_Network::REGTEST)
					{
						devopaddress = CBitcoinAddress("");
					}

					// verify address
					if(devopaddress.IsValid())
					{
						//spork
						if(pindexBest->GetBlockTime() > 1546123500) // ON  (Saturday, December 29, 2018 10:45 PM)
						{
								do_payee = GetScriptForDestination(devopaddress.Get());
						}
						else
						{
							hasPayment = false;
						}
					}
					else
					{
						LogPrintf("CreateNewBlock(): Failed to detect dev address to pay\n");
					}

					if(bMasterNodePayment)
					{
												//spork
						if(!masternodePayments.GetBlockPayee(pindexPrev->nHeight+1, mn_payee, vin))
						{
							CMasternode* winningNode = mnodeman.GetCurrentMasterNode(1);
							if(winningNode)
							{
								mn_payee = GetScriptForDestination(winningNode->pubkey.GetID());
							}
							else
							{
								mn_payee = do_payee;
							}
						}
					}
					else
					{
						hasPayment = false;
					}

					CAmount masternodePayment = GetMasternodePayment(nHeight, blockReward);
					CAmount devopsPayment = GetDevOpsPayment(nHeight, blockReward);

					if (hasPayment)
					{
						pblock->vtx[0].vout.resize(3);
						pblock->vtx[0].vout[1].scriptPubKey = mn_payee;
						pblock->vtx[0].vout[1].nValue = masternodePayment;
						pblock->vtx[0].vout[2].scriptPubKey = do_payee;
						pblock->vtx[0].vout[2].nValue = devopsPayment;
						pblock->vtx[0].vout[0].nValue = blockReward - (masternodePayment + devopsPayment);
					}

					CTxDestination address1;
					CTxDestination address3;
					ExtractDestination(mn_payee, address1);
					ExtractDestination(do_payee, address3);
					CBitcoinAddress address2(address1);
					CBitcoinAddress address4(address3);
					
					LogPrintf("CreateNewBlock(): Masternode payment %lld to %s\n",
						FormatMoney(masternodePayment),
						address2.ToString().c_str()
					);
					
					LogPrintf("CreateNewBlock(): Devops payment %lld to %s\n",
						FormatMoney(devopsPayment),
						address4.ToString().c_str()
					);
				}
			} //
		}

		if (pFees)
		{
			*pFees = nFees;
		}
		
		// Fill in header
		pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
		pblock->nTime          = std::max(pindexPrev->GetPastTimeLimit()+1, pblock->GetMaxTransactionTime());
		
		if (!fProofOfStake)
		{
			pblock->UpdateTime(pindexPrev);
		}
		
		pblock->nNonce         = 0;
	}

	return pblock.release();
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
	// Update nExtraNonce
	static uint256 hashPrevBlock;
	if (hashPrevBlock != pblock->hashPrevBlock)
	{
		nExtraNonce = 0;
		hashPrevBlock = pblock->hashPrevBlock;
	}

	++nExtraNonce;

	unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
	pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CBigNum(nExtraNonce)) + COINBASE_FLAGS;

	assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);

	pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}

void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
	//
	// Pre-build hash buffers
	//
	struct
	{
		struct unnamed2
		{
			int nVersion;
			uint256 hashPrevBlock;
			uint256 hashMerkleRoot;
			unsigned int nTime;
			unsigned int nBits;
			unsigned int nNonce;
		}
		block;
		unsigned char pchPadding0[64];
		uint256 hash1;
		unsigned char pchPadding1[64];
	}
	tmp;

	memset(&tmp, 0, sizeof(tmp));

	tmp.block.nVersion       = pblock->nVersion;
	tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
	tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
	tmp.block.nTime          = pblock->nTime;
	tmp.block.nBits          = pblock->nBits;
	tmp.block.nNonce         = pblock->nNonce;

	FormatHashBlocks(&tmp.block, sizeof(tmp.block));
	FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

	// Byte swap all the input buffer
	for (unsigned int i = 0; i < sizeof(tmp)/4; i++)
	{
		((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);
	}

	// Precalc the first half of the first hash, which stays constant
	SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

	memcpy(pdata, &tmp.block, 128);
	memcpy(phash1, &tmp.hash1, 64);
}

bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
	uint256 hashBlock = pblock->GetHash();
	uint256 hashProof = pblock->GetPoWHash();
	uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

	if(!pblock->IsProofOfWork())
	{
		return error("CheckWork() : %s is not a proof-of-work block", hashBlock.GetHex());
	}

	if (hashProof > hashTarget)
	{
		return error("CheckWork() : proof-of-work not meeting target");
	}

	//// debug print
	LogPrintf("CheckWork() : new proof-of-work block found  \n  proof hash: %s  \ntarget: %s\nblock hash: %s\ngenerated %s\n",
		hashProof.GetHex(),
		hashTarget.GetHex(),
		pblock->ToString(),
		FormatMoney(pblock->vtx[0].vout[0].nValue)
	);

	// Found a solution
	{
		LOCK(cs_main);
		
		if (pblock->hashPrevBlock != hashBestChain)
		{
			return error("CheckWork() : generated block is stale");
		}
		
		// Remove key from key pool
		reservekey.KeepKey();

		// Track how many getdata requests this block gets
		{
			LOCK(wallet.cs_wallet);
			
			wallet.mapRequestCount[hashBlock] = 0;
		}

		// Process this block the same as if we had received it from another node
		if (!ProcessBlock(NULL, pblock))
		{
			return error("CheckWork() : ProcessBlock, block not accepted");
		}
	}

	return true;
}

bool CheckStake(CBlock* pblock, CWallet& wallet)
{
	uint256 proofHash = 0, hashTarget = 0;
	uint256 hashBlock = pblock->GetHash();

	if(!pblock->IsProofOfStake())
	{
		return error("CheckStake() : %s is not a proof-of-stake block", hashBlock.GetHex());
	}

	// verify hash target and signature of coinstake tx
	if (!CheckProofOfStake(mapBlockIndex[pblock->hashPrevBlock], pblock->vtx[1], pblock->nBits, proofHash, hashTarget))
	{
		return error("CheckStake() : proof-of-stake checking failed");
	}

	//// debug print
	LogPrint("coinstake", "CheckStake() : new proof-of-stake block found  \n  hash: %s \nproofhash: %s  \ntarget: %s\nblock %s\nout %s\n",
		hashBlock.GetHex(),
		proofHash.GetHex(),
		hashTarget.GetHex(),
		pblock->ToString(),
		FormatMoney(pblock->vtx[1].GetValueOut())
	);

	// Found a solution
	{
		LOCK(cs_main);
		
		if (pblock->hashPrevBlock != hashBestChain)
		{
			return error("CheckStake() : generated block is stale");
		}
		
		// Track how many getdata requests this block gets
		{
			LOCK(wallet.cs_wallet);
			
			wallet.mapRequestCount[hashBlock] = 0;
		}

		// Process this block the same as if we had received it from another node
		if (!ProcessBlock(NULL, pblock))
		{
			return error("CheckStake() : ProcessBlock, block not accepted");
		}
		else
		{
			//ProcessBlock successful for PoS. now FixSpentCoins.
			int nMismatchSpent;
			CAmount nBalanceInQuestion;
			wallet.FixSpentCoins(nMismatchSpent, nBalanceInQuestion);
			
			if (nMismatchSpent != 0)
			{
				LogPrintf("PoS mismatched spent coins = %d and balance affects = %d \n", nMismatchSpent, nBalanceInQuestion);
			}
		}
	}

	return true;
}

void ThreadStakeMiner(CWallet *pwallet)
{
	SetThreadPriority(THREAD_PRIORITY_LOWEST);

	// Make this thread recognisable as the mining thread
	RenameThread("DigitalNote-miner");

	CReserveKey reservekey(pwallet);

	bool fTryToSync = true;

	while (true)
	{
		while (pwallet->IsLocked())
		{
			nLastCoinStakeSearchInterval = 0;
			
			MilliSleep(1000);
		}

		while (vNodes.empty() || IsInitialBlockDownload())
		{
			nLastCoinStakeSearchInterval = 0;
			fTryToSync = true;
			
			MilliSleep(1000);
		}

		if (fTryToSync)
		{
			fTryToSync = false;
			
			if (vNodes.size() < 3 || pindexBest->GetBlockTime() < GetTime() - 10 * 60)
			{
				MilliSleep(10000);
				
				continue;
			}
		}

		//
		// Create new block
		//
		int64_t nFees;
		CBlockPtr pblock(CreateNewBlock(reservekey, true, &nFees));
		
		if (!pblock.get())
		{
			return;
		}
		
		// Trying to sign a block
		if (pblock->SignBlock(*pwallet, nFees))
		{
			SetThreadPriority(THREAD_PRIORITY_NORMAL);
			
			CheckStake(pblock.get(), *pwallet);
			
			SetThreadPriority(THREAD_PRIORITY_LOWEST);
			
			MilliSleep(500);
		}
		else
		{
			MilliSleep(nMinerSleep);
		}
	}
}

