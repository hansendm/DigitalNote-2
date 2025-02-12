#include "cscript.h"
#include "chashwriter.h"
#include "cpubkey.h"
#include "serialize.h"
#include "ckeyid.h"
#include "cscriptid.h"
#include "cautofile.h"
#include "cdatastream.h"
#include "cflatdata.h"
#include "cvarint.h"

#include "cscriptcompressor.h"

bool CScriptCompressor::IsToKeyID(CKeyID &hash) const
{
	if (script.size() == 25 &&
		script[0] == OP_DUP &&
		script[1] == OP_HASH160 &&
		script[2] == 20 &&
		script[23] == OP_EQUALVERIFY &&
		script[24] == OP_CHECKSIG)
	{
		memcpy(&hash, &script[3], 20);
		
		return true;
	}

	return false;
}

bool CScriptCompressor::IsToScriptID(CScriptID &hash) const
{
    if (script.size() == 23 &&
		script[0] == OP_HASH160 &&
		script[1] == 20 &&
		script[22] == OP_EQUAL)
	{
        memcpy(&hash, &script[2], 20);
        
		return true;
    }
	
    return false;
}

bool CScriptCompressor::IsToPubKey(CPubKey &pubkey) const
{
    if(script.size() == 35 &&
		script[0] == 33 &&
		script[34] == OP_CHECKSIG &&
		(
			script[1] == 0x02 ||
			script[1] == 0x03
		)
	)
	{
        pubkey.Set(&script[1], &script[34]);
        return true;
    }
	
    if (script.size() == 67 &&
		script[0] == 65 &&
		script[66] == OP_CHECKSIG &&
		script[1] == 0x04)
	{
        pubkey.Set(&script[1], &script[66]);
        
		return pubkey.IsFullyValid(); // if not fully valid, a case that would not be compressible
    }
	
    return false;
}

bool CScriptCompressor::Compress(std::vector<unsigned char> &out) const
{
	CKeyID keyID;

	if (IsToKeyID(keyID))
	{
		out.resize(21);
		out[0] = 0x00;
		
		memcpy(&out[1], &keyID, 20);
		
		return true;
	}

	CScriptID scriptID;

	if (IsToScriptID(scriptID))
	{
		out.resize(21);
		out[0] = 0x01;
		
		memcpy(&out[1], &scriptID, 20);
		
		return true;
	}

	CPubKey pubkey;

	if (IsToPubKey(pubkey))
	{
		out.resize(33);
		
		memcpy(&out[1], &pubkey[1], 32);
		
		if (pubkey[0] == 0x02 || pubkey[0] == 0x03)
		{
			out[0] = pubkey[0];
			
			return true;
		}
		else if (pubkey[0] == 0x04)
		{
			out[0] = 0x04 | (pubkey[64] & 0x01);
			
			return true;
		}
	}

	return false;
}

unsigned int CScriptCompressor::GetSpecialSize(unsigned int nSize) const
{
	if (nSize == 0 || nSize == 1)
	{
		return 20;
	}

	if (nSize == 2 || nSize == 3 || nSize == 4 || nSize == 5)
	{
		return 32;
	}

	return 0;
}

bool CScriptCompressor::Decompress(unsigned int nSize, const std::vector<unsigned char> &in)
{
    switch(nSize) 
	{
		case 0x00:
			script.resize(25);
			script[0] = OP_DUP;
			script[1] = OP_HASH160;
			script[2] = 20;
			
			memcpy(&script[3], &in[0], 20);
			
			script[23] = OP_EQUALVERIFY;
			script[24] = OP_CHECKSIG;
			
			return true;
		case 0x01:
			script.resize(23);
			script[0] = OP_HASH160;
			script[1] = 20;
			
			memcpy(&script[2], &in[0], 20);
			
			script[22] = OP_EQUAL;
			
			return true;
		case 0x02:
		case 0x03:
			script.resize(35);
			script[0] = 33;
			script[1] = nSize;
			
			memcpy(&script[2], &in[0], 32);
			
			script[34] = OP_CHECKSIG;
			
			return true;
		case 0x04:
		case 0x05:
			unsigned char vch[33] = {};
			vch[0] = nSize - 2;
			
			memcpy(&vch[1], &in[0], 32);
			
			CPubKey pubkey(&vch[0], &vch[33]);
			if (!pubkey.Decompress())
			{
				return false;
			}
			
			assert(pubkey.size() == 65);
			
			script.resize(67);
			script[0] = 65;
			
			memcpy(&script[1], pubkey.begin(), 65);
			
			script[66] = OP_CHECKSIG;
			
			return true;
    }
	
    return false;
}

CScriptCompressor::CScriptCompressor(CScript &scriptIn) : script(scriptIn)
{
	
}

unsigned int CScriptCompressor::GetSerializeSize(int nType, int nVersion) const
{
	std::vector<unsigned char> compr;
	
	if (Compress(compr))
	{
		return compr.size();
	}
	
	unsigned int nSize = script.size() + nSpecialScripts;
	
	return script.size() + VARINT(nSize).GetSerializeSize(nType, nVersion);
}

template<typename Stream>
void CScriptCompressor::Serialize(Stream &s, int nType, int nVersion) const
{
	std::vector<unsigned char> compr;
	
	if (Compress(compr))
	{
		s << CFlatData(&compr[0], &compr[compr.size()]);
		
		return;
	}
	
	unsigned int nSize = script.size() + nSpecialScripts;
	
	s << VARINT(nSize);
	s << CFlatData(&script[0], &script[script.size()]);
}

template<typename Stream>
void CScriptCompressor::Unserialize(Stream &s, int nType, int nVersion)
{
	unsigned int nSize;
	
	s >> VARINT(nSize);
	
	if (nSize < nSpecialScripts)
	{
		std::vector<unsigned char> vch(GetSpecialSize(nSize), 0x00);
		
		s >> REF(CFlatData(&vch[0], &vch[vch.size()]));
		
		Decompress(nSize, vch);
		
		return;
	}
	
	nSize -= nSpecialScripts;
	
	script.resize(nSize);
	
	s >> REF(CFlatData(&script[0], &script[script.size()]));
}

template void CScriptCompressor::Serialize<CDataStream>(CDataStream& s, int nType, int nVersion) const;
template void CScriptCompressor::Unserialize<CDataStream>(CDataStream& s, int nType, int nVersion);
template void CScriptCompressor::Serialize<CAutoFile>(CAutoFile& s, int nType, int nVersion) const;
template void CScriptCompressor::Unserialize<CAutoFile>(CAutoFile& s, int nType, int nVersion);
template void CScriptCompressor::Serialize<CHashWriter>(CHashWriter& s, int nType, int nVersion) const;

