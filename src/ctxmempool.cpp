#include "ctransaction.h"
#include "cinpoint.h"
#include "ctxout.h"
#include "ctxin.h"
#include "thread.h"

#include "ctxmempool.h"

CTxMemPool::CTxMemPool()
{
	
}

unsigned int CTxMemPool::GetTransactionsUpdated() const
{
	LOCK(cs);

	return nTransactionsUpdated;
}

void CTxMemPool::AddTransactionsUpdated(unsigned int n)
{
	LOCK(cs);

	nTransactionsUpdated += n;
}

bool CTxMemPool::addUnchecked(const uint256& hash, CTransaction &tx)
{
	// Add to memory pool without checking anything.
	// Used by main.cpp AcceptToMemoryPool(), which DOES do
	// all the appropriate checks.
	LOCK(cs);

	{
		mapTx[hash] = tx;
		
		for (unsigned int i = 0; i < tx.vin.size(); i++)
		{
			mapNextTx[tx.vin[i].prevout] = CInPoint(&mapTx[hash], i);
		}
		
		nTransactionsUpdated++;
	}

	return true;
}

bool CTxMemPool::remove(const CTransaction &tx, bool fRecursive)
{
	// Remove transaction from memory pool
	{
		LOCK(cs);
		
		uint256 hash = tx.GetHash();
		
		if (mapTx.count(hash))
		{
			if (fRecursive)
			{
				for (unsigned int i = 0; i < tx.vout.size(); i++)
				{
					std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(COutPoint(hash, i));
					
					if (it != mapNextTx.end())
					{
						remove(*it->second.ptx, true);
					}
				}
			}
			
			for(const CTxIn& txin : tx.vin)
			{
				mapNextTx.erase(txin.prevout);
			}
			
			mapTx.erase(hash);
			nTransactionsUpdated++;
		}
	}

	return true;
}

bool CTxMemPool::removeConflicts(const CTransaction &tx)
{
	// Remove transactions which depend on inputs of tx, recursively
	LOCK(cs);

	for(const CTxIn &txin : tx.vin)
	{
		std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(txin.prevout);
		if (it != mapNextTx.end())
		{
			const CTransaction &txConflict = *it->second.ptx;
			
			if (txConflict != tx)
			{
				remove(txConflict, true);
			}
		}
	}

	return true;
}

void CTxMemPool::clear()
{
	LOCK(cs);
	
	mapTx.clear();
	mapNextTx.clear();
	
	++nTransactionsUpdated;
}

void CTxMemPool::queryHashes(std::vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
	
    vtxid.reserve(mapTx.size());
	
    for (std::map<uint256, CTransaction>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
	{
        vtxid.push_back((*mi).first);
	}
}

unsigned long CTxMemPool::size() const
{
	LOCK(cs);
	
	return mapTx.size();
}

bool CTxMemPool::exists(uint256 hash) const
{
	LOCK(cs);
	
	return (mapTx.count(hash) != 0);
}

bool CTxMemPool::lookup(uint256 hash, CTransaction& result) const
{
    LOCK(cs);
	
    std::map<uint256, CTransaction>::const_iterator i = mapTx.find(hash);
    
	if (i == mapTx.end())
	{
		return false;
	}
	
    result = i->second;
    
	return true;
}

