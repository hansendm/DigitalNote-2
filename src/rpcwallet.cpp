#include "compat.h"

#include "enums/rpcerrorcode.h"
#include "base58.h"
#include "blockparams.h"
#include "stealth.h"
#include "rpcserver.h"
#include "init.h"
#include "net.h"
#include "util.h"
#include "walletdb.h"
#include "cwallet.h"
#include "cwallettx.h"
#include "caccount.h"
#include "caccountingentry.h"
#include "creservekey.h"
#include "cblock.h"
#include "cblockindex.h"
#include "cblocklocator.h"
#include "coutput.h"
#include "mining.h"
#include "wallet.h"
#include "script.h"
#include "main_extern.h"
#include "webwalletconnector.h"
#include "chashwriter.h"
#include "thread.h"
#include "ckey.h"
#include "ctxout.h"
#include "ctxin.h"
#include "cdigitalnoteaddress.h"
#include "cnodestination.h"
#include "ckeyid.h"
#include "cscriptid.h"
#include "cstealthaddress.h"
#include "enums/serialize_type.h"
#include "rpcprotocol.h"
#include "rpcrawtransaction.h"
#include "tallyitem.h"

typedef std::map<std::string, tallyitem> mapAccountTally_t;

int64_t nWalletUnlockTime;
static CCriticalSection cs_nWalletUnlockTime;

static void accountingDeprecationCheck()
{
	if (!GetBoolArg("-enableaccounts", false))
	{
		throw std::runtime_error(
			"Accounting API is deprecated and will be removed in future.\n"
			"It can easily result in negative or odd balances if misused or misunderstood, which has happened in the field.\n"
			"If you still want to enable it, add to your config file enableaccounts=1\n"
		);
	}

	if (GetBoolArg("-staking", true))
	{
		throw std::runtime_error("If you want to use accounting API, staking must be disabled, add to your config file staking=0\n");
	}
}

std::string HelpRequiringPassphrase()
{
	return pwalletMain && pwalletMain->IsCrypted()
		? "\nrequires wallet passphrase to be set with walletpassphrase first" : "";
}

void EnsureWalletIsUnlocked()
{
	if (pwalletMain->IsLocked())
	{
		throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
	}

	if (fWalletUnlockStakingOnly)
	{
		throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet is unlocked for staking only.");
	}
}

void WalletTxToJSON(const CWalletTx& wtx, json_spirit::Object& entry)
{
	int confirms = wtx.GetDepthInMainChain(false);
	int confirmsTotal = GetIXConfirmations(wtx.GetHash()) + confirms;

	entry.push_back(json_spirit::Pair("confirmations", confirmsTotal));
	entry.push_back(json_spirit::Pair("bcconfirmations", confirms));

	if (wtx.IsCoinBase() || wtx.IsCoinStake())
	{
		entry.push_back(json_spirit::Pair("generated", true));
	}

	if (confirms > 0)
	{
		entry.push_back(json_spirit::Pair("blockhash", wtx.hashBlock.GetHex()));
		entry.push_back(json_spirit::Pair("blockindex", wtx.nIndex));
		entry.push_back(json_spirit::Pair("blocktime", (int64_t)(mapBlockIndex[wtx.hashBlock]->nTime)));
	}

	uint256 hash = wtx.GetHash();
	json_spirit::Array conflicts;

	entry.push_back(json_spirit::Pair("txid", hash.GetHex()));

	for(const uint256& conflict : wtx.GetConflicts())
	{
		conflicts.push_back(conflict.GetHex());
	}

	entry.push_back(json_spirit::Pair("walletconflicts", conflicts));
	entry.push_back(json_spirit::Pair("time", wtx.GetTxTime()));
	entry.push_back(json_spirit::Pair("timereceived", (int64_t)wtx.nTimeReceived));

	for(const std::pair<std::string, std::string>& item : wtx.mapValue)
	{
		entry.push_back(json_spirit::Pair(item.first, item.second));
	}
}

std::string AccountFromValue(const json_spirit::Value& value)
{
	std::string strAccount = value.get_str();

	if (strAccount == "*")
	{
		throw JSONRPCError(RPC_WALLET_INVALID_ACCOUNT_NAME, "Invalid account name");
	}

	return strAccount;
}

//
// Used by addmultisigaddress / createmultisig:
//
CScript _createmultisig(const json_spirit::Array& params)
{
	int nRequired = params[0].get_int();
	const json_spirit::Array& keys = params[1].get_array();

	// Gather public keys
	if (nRequired < 1)
	{
		throw std::runtime_error("a multisignature address must require at least one key to redeem");
	}

	if ((int)keys.size() < nRequired)
	{
		throw std::runtime_error(
			strprintf(
				"not enough keys supplied (got %" PRIszu" keys, but need at least %d to redeem)",
				keys.size(),
				nRequired
			)
		);
	}

	std::vector<CPubKey> pubkeys;
	pubkeys.resize(keys.size());

	for (unsigned int i = 0; i < keys.size(); i++)
	{
		const std::string& ks = keys[i].get_str();

#ifdef ENABLE_WALLET
		// Case 1: DigitalNote address and we have full public key:
		CDigitalNoteAddress address(ks);
		
		if (pwalletMain && address.IsValid())
		{
			CKeyID keyID;
			
			if (!address.GetKeyID(keyID))
			{
				throw std::runtime_error(strprintf("%s does not refer to a key",ks));
			}
			
			CPubKey vchPubKey;
			
			if (!pwalletMain->GetPubKey(keyID, vchPubKey))
			{
				throw std::runtime_error(strprintf("no full public key for address %s",ks));
			}
			
			if (!vchPubKey.IsFullyValid())
			{
				throw std::runtime_error(" Invalid public key: "+ks);
			}
			
			pubkeys[i] = vchPubKey;
		}
		else // Case 2: hex public key
#endif // ENABLE_WALLET

		if (IsHex(ks))
		{
			CPubKey vchPubKey(ParseHex(ks));
			
			if (!vchPubKey.IsFullyValid())
			{
				throw std::runtime_error(" Invalid public key: "+ks);
			}
			
			pubkeys[i] = vchPubKey;
		}
		else
		{
			throw std::runtime_error(" Invalid public key: "+ks);
		}
	}

	CScript result;

	result.SetMultisig(nRequired, pubkeys);

	return result;
}

json_spirit::Value createmultisig(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 2 || params.size() > 2)
	{
		std::string msg = "createmultisig nrequired [\"key\",...]\n"
			"\nCreates a multi-signature address with n signature of m keys required.\n"
			"It returns a json object with the address and redeemScript.\n"

			"\nArguments:\n"
			"1. nrequired (numeric, required) The number of required signatures out of the n keys or addresses.\n"
			"2. \"keys\" (string, required) A json array of keys which are bitcoin addresses or hex-encoded public keys\n"
			" [\n"
			" \"key\" (string) bitcoin address or hex-encoded public key\n"
			" ,...\n"
			" ]\n"

			"\nResult:\n"
			"{\n"
			" \"address\":\"multisigaddress\", (string) The value of the new multisig address.\n"
			" \"redeemScript\":\"script\" (string) The string value of the hex-encoded redemption script.\n"
			"}\n"

			"\nExamples:\n"
			"\nCreate a multisig address from 2 addresses\n"
			+ HelpExampleCli("createmultisig", "2 \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"") +
			"\nAs a json rpc call\n"
			+ HelpExampleRpc("createmultisig", "2, \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"")
		;
		throw std::runtime_error(msg);
	}

	// Construct using pay-to-script-hash:
	CScript inner = _createmultisig(params);
	CScriptID innerID = inner.GetID();
	CDigitalNoteAddress address(innerID);

	json_spirit::Object result;
	result.push_back(json_spirit::Pair("address", address.ToString()));
	result.push_back(json_spirit::Pair("redeemScript", HexStr(inner.begin(), inner.end())));

	return result;
}

json_spirit::Value getnewpubkey(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() > 1)
	{
		throw std::runtime_error(
			"getnewpubkey [account]\n"
			"Returns new public key for coinbase generation."
		);
	}

	// Parse the account first so we don't generate a key if there's an error
	std::string strAccount;
	if (params.size() > 0)
	{
		strAccount = AccountFromValue(params[0]);
	}

	if (!pwalletMain->IsLocked())
	{
		pwalletMain->TopUpKeyPool();
	}

	// Generate a new key that is added to wallet
	CPubKey newKey;
	if (!pwalletMain->GetKeyFromPool(newKey))
	{
		throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
	}

	CKeyID keyID = newKey.GetID();

	pwalletMain->SetAddressBookName(keyID, strAccount);

	return HexStr(newKey.begin(), newKey.end());
}

json_spirit::Value getnewaddress(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() > 1)
	{
		throw std::runtime_error(
			"getnewaddress ( \"account\" )\n"
			"\nReturns a new DigitalNote address for receiving payments.\n"
			"If 'account' is specified (recommended), it is added to the address book \n"
			"so payments received with the address will be credited to 'account'.\n"
			"\nArguments:\n"
			"1. \"account\"        (string, optional) The account name for the address to be linked to. if not provided, the default account \"\" is used. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created if there is no account by the given name.\n"
			"\nResult:\n"
			"\"DigitalNote\"    (string) The new DigitalNote address\n"
			"\nExamples:\n"
			+ HelpExampleCli("getnewaddress", "")
			+ HelpExampleCli("getnewaddress", "\"\"")
			+ HelpExampleCli("getnewaddress", "\"myaccount\"")
			+ HelpExampleRpc("getnewaddress", "\"myaccount\"")
		);
	}

	// Parse the account first so we don't generate a key if there's an error
	std::string strAccount;
	if (params.size() > 0)
	{
		strAccount = AccountFromValue(params[0]);
	}

	if (!pwalletMain->IsLocked())
	{
		pwalletMain->TopUpKeyPool();
	}

	// Generate a new key that is added to wallet
	CPubKey newKey;

	if (!pwalletMain->GetKeyFromPool(newKey))
	{
		throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
	}

	CKeyID keyID = newKey.GetID();

	pwalletMain->SetAddressBookName(keyID, strAccount);

	return CDigitalNoteAddress(keyID).ToString();
}

CDigitalNoteAddress GetAccountAddress(const std::string &strAccount, bool bForceNew=false)
{
	CWalletDB walletdb(pwalletMain->strWalletFile);

	CAccount account;
	walletdb.ReadAccount(strAccount, account);

	bool bKeyUsed = false;

	// Check if the current key has been used
	if (account.vchPubKey.IsValid())
	{
		CScript scriptPubKey;
		scriptPubKey.SetDestination(account.vchPubKey.GetID());
		
		for (mapWallet_t::iterator it = pwalletMain->mapWallet.begin();
			 it != pwalletMain->mapWallet.end() && account.vchPubKey.IsValid(); ++it)
		{
			const CWalletTx& wtx = (*it).second;
			
			for(const CTxOut& txout : wtx.vout)
			{
				if (txout.scriptPubKey == scriptPubKey)
				{
					bKeyUsed = true;
				}
			}
		}
	}

	// Generate a new key
	if (!account.vchPubKey.IsValid() || bForceNew || bKeyUsed)
	{
		if (!pwalletMain->GetKeyFromPool(account.vchPubKey))
		{
			throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
		}
		
		pwalletMain->SetAddressBookName(account.vchPubKey.GetID(), strAccount);
		walletdb.WriteAccount(strAccount, account);
	}

	return CDigitalNoteAddress(account.vchPubKey.GetID());
}

json_spirit::Value getaccountaddress(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() != 1)
	{
		throw std::runtime_error(
			"getaccountaddress \"account\"\n"
			"\nReturns the current DigitalNote address for receiving payments to this account.\n"
			"\nArguments:\n"
			"1. \"account\"       (string, required) The account name for the address. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created and a new address created  if there is no account by the given name.\n"
			"\nResult:\n"
			"\"DigitalNote\"   (string) The account DigitalNote address\n"
			"\nExamples:\n"
			+ HelpExampleCli("getaccountaddress", "")
			+ HelpExampleCli("getaccountaddress", "\"\"")
			+ HelpExampleCli("getaccountaddress", "\"myaccount\"")
			+ HelpExampleRpc("getaccountaddress", "\"myaccount\"")
		);
	}

	// Parse the account first so we don't generate a key if there's an error
	std::string strAccount = AccountFromValue(params[0]);

	json_spirit::Value ret;

	ret = GetAccountAddress(strAccount).ToString();

	return ret;
}

json_spirit::Value setaccount(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 1 || params.size() > 2)
	{
		throw std::runtime_error(
			"setaccount \"DigitalNote\" \"account\"\n"
			"\nSets the account associated with the given address.\n"
			"\nArguments:\n"
			"1. \"DigitalNoteaddress\"  (string, required) The DigitalNote address to be associated with an account.\n"
			"2. \"account\"         (string, required) The account to assign the address to.\n"
			"\nExamples:\n"
			+ HelpExampleCli("setaccount", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" \"tabby\"")
			+ HelpExampleRpc("setaccount", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\", \"tabby\"")
		);
	}

	CDigitalNoteAddress address(params[0].get_str());
	if (!address.IsValid())
	{
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid DigitalNote address");
	}

	std::string strAccount;
	if (params.size() > 1)
	{
		strAccount = AccountFromValue(params[1]);
	}

	// Only add the account if the address is yours.
	if (IsMine(*pwalletMain, address.Get()))
	{
		// Detect when changing the account of an address that is the 'unused current key' of another account:
		if (pwalletMain->mapAddressBook.count(address.Get()))
		{
			std::string strOldAccount = pwalletMain->mapAddressBook[address.Get()];
			
			if (address == GetAccountAddress(strOldAccount))
			{
				GetAccountAddress(strOldAccount, true);
			}
		}
		
		if (fWebWalletMode)
		{
			pwalletMain->SetAddressAccountIdAssociation(address.Get(), strAccount);
		}
		else
		{
			pwalletMain->SetAddressBookName(address.Get(), strAccount);
		}
	}
	else
	{
		throw JSONRPCError(RPC_MISC_ERROR, "setaccount can only be used with own address");
	}

	return json_spirit::Value::null;
}

json_spirit::Value getaccount(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() != 1)
	{
		throw std::runtime_error(
			"getaccount \"DigitalNote\"\n"
			"\nReturns the account associated with the given address.\n"
			"\nArguments:\n"
			"1. \"DigitalNote\"  (string, required) The DigitalNote address for account lookup.\n"
			"\nResult:\n"
			"\"accountname\"        (string) the account address\n"
			"\nExamples:\n"
			+ HelpExampleCli("getaccount", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\"")
			+ HelpExampleRpc("getaccount", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\"")
		);
	}

	CDigitalNoteAddress address(params[0].get_str());

	if (!address.IsValid())
	{
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid DigitalNote address");
	}

	std::string strAccount;
	mapAddressBook_t::iterator mi = pwalletMain->mapAddressBook.find(address.Get());

	if (mi != pwalletMain->mapAddressBook.end() && !(*mi).second.empty())
	{
		strAccount = (*mi).second;
	}

	return strAccount;
}

json_spirit::Value getaddressesbyaccount(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() != 1)
	{
		throw std::runtime_error(
			"getaddressesbyaccount \"account\"\n"
			"\nReturns the list of addresses for the given account.\n"
			"\nArguments:\n"
			"1. \"account\"  (string, required) The account name.\n"
			"\nResult:\n"
			"[                     (json array of string)\n"
			"  \"DigitalNote\"  (string) a DigitalNote address associated with the given account\n"
			"  ,...\n"
			"]\n"
			"\nExamples:\n"
			+ HelpExampleCli("getaddressesbyaccount", "\"tabby\"")
			+ HelpExampleRpc("getaddressesbyaccount", "\"tabby\"")
		);
	}

	std::string strAccount = AccountFromValue(params[0]);

	// Find all addresses that have the given account
	json_spirit::Array ret;

	for(const std::pair<CDigitalNoteAddress, std::string>& item : pwalletMain->mapAddressBook)
	{
		const CDigitalNoteAddress& address = item.first;
		const std::string& strName = item.second;
		
		if (strName == strAccount)
		{
			ret.push_back(address.ToString());
		}
	}

	return ret;
}

json_spirit::Value sendtoaddress(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 2 || params.size() > 4)
	{
		throw std::runtime_error(
			"sendtoaddress \"DigitalNote\" amount ( \"comment\" \"comment-to\" )\n"
			"\nSent an amount to a given address. The amount is a real and is rounded to the nearest 0.00000001\n"
			+ HelpRequiringPassphrase() +
			"\nArguments:\n"
		   "1. \"DigitalNote\"  (string, required) The DigitalNote address to send to.\n"
			"2. \"amount\"      (numeric, required) The amount in XDN to send. eg 0.1\n"
			"3. \"comment\"     (string, optional) A comment used to store what the transaction is for. \n"
			"                             This is not part of the transaction, just kept in your wallet.\n"
			"4. \"comment-to\"  (string, optional) A comment to store the name of the person or organization \n"
			"                             to which you're sending the transaction. This is not part of the \n"
			"                             transaction, just kept in your wallet.\n"
			"\nResult:\n"
			"\"transactionid\"  (string) The transaction id.\n"
			"\nExamples:\n"
			+ HelpExampleCli("sendtoaddress", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" 0.1")
			+ HelpExampleCli("sendtoaddress", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" 0.1 \"donation\" \"seans outpost\"")
			+ HelpExampleRpc("sendtoaddress", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\", 0.1, \"donation\", \"seans outpost\"")
		);
	}

	EnsureWalletIsUnlocked();

	if (params[0].get_str().length() > 75 && IsStealthAddress(params[0].get_str()))
	{
		return sendtostealthaddress(params, false);
	}

	CDigitalNoteAddress address(params[0].get_str());
	if (!address.IsValid())
	{
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid DigitalNote address");
	}

	// Amount
	CAmount nAmount = AmountFromValue(params[1]);

	CWalletTx wtx;
	std::string sNarr;

	// Wallet comments
	if (params.size() > 2 && params[2].type() != json_spirit::null_type && !params[2].get_str().empty())
	{
		wtx.mapValue["comment"] = params[2].get_str();
	}

	if (params.size() > 3 && params[3].type() != json_spirit::null_type && !params[3].get_str().empty())
	{
		wtx.mapValue["to"]      = params[3].get_str();
	}

	if (params.size() > 4 && params[4].type() != json_spirit::null_type && !params[4].get_str().empty())
	{
		sNarr = params[4].get_str();
	}

	if (sNarr.length() > 24)
	{
		throw std::runtime_error("Narration must be 24 characters or less.");
	}

	std::string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, sNarr, wtx);

	if (strError != "")
	{
		throw JSONRPCError(RPC_WALLET_ERROR, strError);
	}

	return wtx.GetHash().GetHex();
}

json_spirit::Value listaddressgroupings(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp)
	{
		throw std::runtime_error(
			"listaddressgroupings\n"
			"\nLists groups of addresses which have had their common ownership\n"
			"made public by common use as inputs or as the resulting change\n"
			"in past transactions\n"
			"\nResult:\n"
			"[\n"
			"  [\n"
			"    [\n"
			"      \"DigitalNote\",     (string) The DigitalNote address\n"
			"      amount,                 (numeric) The amount in XDN\n"
			"      \"account\"             (string, optional) The account\n"
			"    ]\n"
			"    ,...\n"
			"  ]\n"
			"  ,...\n"
			"]\n"
			"\nExamples:\n"
			+ HelpExampleCli("listaddressgroupings", "")
			+ HelpExampleRpc("listaddressgroupings", "")
		);
	}

	json_spirit::Array jsonGroupings;
	std::map<CTxDestination, int64_t> balances = pwalletMain->GetAddressBalances();

	for(std::set<CTxDestination> grouping : pwalletMain->GetAddressGroupings())
	{
		json_spirit::Array jsonGrouping;
		
		for(CTxDestination address : grouping)
		{
			json_spirit::Array addressInfo;
			
			addressInfo.push_back(CDigitalNoteAddress(address).ToString());
			addressInfo.push_back(ValueFromAmount(balances[address]));
			
			{
				LOCK(pwalletMain->cs_wallet);
				
				if (pwalletMain->mapAddressBook.find(CDigitalNoteAddress(address).Get()) != pwalletMain->mapAddressBook.end())
				{
					addressInfo.push_back(pwalletMain->mapAddressBook.find(CDigitalNoteAddress(address).Get())->second);
				}
			}
			
			jsonGrouping.push_back(addressInfo);
		}
		
		jsonGroupings.push_back(jsonGrouping);
	}

	return jsonGroupings;
}

json_spirit::Value signmessage(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() != 2)
	{
		throw std::runtime_error(
			"signmessage \"DigitalNote\" \"message\"\n"
			"\nSign a message with the private key of an address"
			+ HelpRequiringPassphrase() + "\n"
			"\nArguments:\n"
			"1. \"DigitalNote\"  (string, required) The DigitalNote address to use for the private key.\n"
			"2. \"message\"         (string, required) The message to create a signature of.\n"
			"\nResult:\n"
			"\"signature\"          (string) The signature of the message encoded in base 64\n"
			"\nExamples:\n"
			"\nUnlock the wallet for 30 seconds\n"
			+ HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
			"\nCreate the signature\n"
			+ HelpExampleCli("signmessage", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" \"my message\"") +
			"\nVerify the signature\n"
			+ HelpExampleCli("verifymessage", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" \"signature\" \"my message\"") +
			"\nAs json rpc\n"
			+ HelpExampleRpc("signmessage", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\", \"my message\"")
		);
	}

	EnsureWalletIsUnlocked();

	std::string strAddress = params[0].get_str();
	std::string strMessage = params[1].get_str();

	CDigitalNoteAddress addr(strAddress);
	if (!addr.IsValid())
	{
		throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
	}

	CKeyID keyID;
	if (!addr.GetKeyID(keyID))
	{
		throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
	}

	CKey key;
	if (!pwalletMain->GetKey(keyID, key))
	{
		throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
	}

	CHashWriter ss(SER_GETHASH, 0);

	ss << strMessageMagic;
	ss << strMessage;

	std::vector<unsigned char> vchSig;
	if (!key.SignCompact(ss.GetHash(), vchSig))
	{
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");
	}

	return EncodeBase64(&vchSig[0], vchSig.size());
}

json_spirit::Value getreceivedbyaddress(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 1 || params.size() > 2)
	{
		throw std::runtime_error(
			"getreceivedbyaddress \"DigitalNote\" ( minconf )\n"
			"\nReturns the total amount received by the given DigitalNote in transactions with at least minconf confirmations.\n"
			"\nArguments:\n"
			"1. \"DigitalNote\"  (string, required) The DigitalNote address for transactions.\n"
			"2. minconf             (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
			"\nResult:\n"
			"amount   (numeric) The total amount in XDN received at this address.\n"
			"\nExamples:\n"
			"\nThe amount from transactions with at least 1 confirmation\n"
			+ HelpExampleCli("getreceivedbyaddress", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\"") +
			"\nThe amount including unconfirmed transactions, zero confirmations\n"
			+ HelpExampleCli("getreceivedbyaddress", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" 0") +
			"\nThe amount with at least 10 confirmation, very safe\n"
			+ HelpExampleCli("getreceivedbyaddress", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" 10") +
			"\nAs a json rpc call\n"
			+ HelpExampleRpc("getreceivedbyaddress", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\", 10")
	   );
	}

	// DigitalNote address
	CDigitalNoteAddress address = CDigitalNoteAddress(params[0].get_str());
	CScript scriptPubKey;
	if (!address.IsValid())
	{
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid DigitalNote address");
	}

	scriptPubKey.SetDestination(address.Get());

	if (!IsMine(*pwalletMain,scriptPubKey))
	{
		return (double)0.0;
	}

	// Minimum confirmations
	int nMinDepth = 1;
	if (params.size() > 1)
	{
		nMinDepth = params[1].get_int();
	}

	// Tally
	CAmount nAmount = 0;
	for (mapWallet_t::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
	{
		const CWalletTx& wtx = (*it).second;
		if (wtx.IsCoinBase() || wtx.IsCoinStake() || !IsFinalTx(wtx))
		{
			continue;
		}
		
		for(const CTxOut& txout : wtx.vout)
		{
			if (txout.scriptPubKey == scriptPubKey && wtx.GetDepthInMainChain() >= nMinDepth)
			{
				nAmount += txout.nValue;
			}
		}
	}

	return  ValueFromAmount(nAmount);
}

void GetAccountAddresses(const std::string &strAccount, std::set<CTxDestination>& setAddress)
{
	for(const pairAddressBook_t& item : pwalletMain->mapAddressBook)
	{
		const CTxDestination& address = item.first;
		const std::string& strName = item.second;
		
		if (strName == strAccount)
		{
			setAddress.insert(address);
		}
	}
}

json_spirit::Value getreceivedbyaccount(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 1 || params.size() > 2)
	{
		throw std::runtime_error(
			"getreceivedbyaccount \"account\" ( minconf )\n"
			"\nReturns the total amount received by addresses with <account> in transactions with at least [minconf] confirmations.\n"
			"\nArguments:\n"
			"1. \"account\"      (string, required) The selected account, may be the default account using \"\".\n"
			"2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
			"\nResult:\n"
			"amount              (numeric) The total amount in XDN received for this account.\n"
			"\nExamples:\n"
			"\nAmount received by the default account with at least 1 confirmation\n"
			+ HelpExampleCli("getreceivedbyaccount", "\"\"") +
			"\nAmount received at the tabby account including unconfirmed amounts with zero confirmations\n"
			+ HelpExampleCli("getreceivedbyaccount", "\"tabby\" 0") +
			"\nThe amount with at least 10 confirmation, very safe\n"
			+ HelpExampleCli("getreceivedbyaccount", "\"tabby\" 10") +
			"\nAs a json rpc call\n"
			+ HelpExampleRpc("getreceivedbyaccount", "\"tabby\", 10")
		);
	}

	accountingDeprecationCheck();

	// Minimum confirmations
	int nMinDepth = 1;
	if (params.size() > 1)
	{
		nMinDepth = params[1].get_int();
	}

	// Get the set of pub keys assigned to account
	std::string strAccount = AccountFromValue(params[0]);
	std::set<CTxDestination> setAddress;

	GetAccountAddresses(strAccount, setAddress);

	// Tally
	CAmount nAmount = 0;
	for (mapWallet_t::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
	{
		const CWalletTx& wtx = (*it).second;
		
		if (wtx.IsCoinBase() || wtx.IsCoinStake() || !IsFinalTx(wtx))
		{
			continue;
		}
		
		for(const CTxOut& txout : wtx.vout)
		{
			CTxDestination address;
			
			if (ExtractDestination(txout.scriptPubKey, address) &&
				IsMine(*pwalletMain, address) &&
				setAddress.count(address) && 
				wtx.GetDepthInMainChain() >= nMinDepth
			)
			{
				nAmount += txout.nValue;
			}
		}
	}

	return (double)nAmount / (double)COIN;
}

int64_t GetAccountBalance(CWalletDB& walletdb, const std::string& strAccount, int nMinDepth, const isminefilter& filter)
{
	int64_t nBalance = 0;

	// Tally wallet transactions
	for (mapWallet_t::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
	{
		const CWalletTx& wtx = (*it).second;
		if (!IsFinalTx(wtx) || wtx.GetBlocksToMaturity() > 0 || wtx.GetDepthInMainChain() < 0)
		{
			continue;
		}
		
		int64_t nReceived, nSent, nFee;
		wtx.GetAccountAmounts(strAccount, nReceived, nSent, nFee, filter);

		if (nReceived != 0 && wtx.GetDepthInMainChain() >= nMinDepth)
		{
			nBalance += nReceived;
		}
		
		nBalance -= nSent + nFee;
	}

	// Tally internal accounting entries
	nBalance += walletdb.GetAccountCreditDebit(strAccount);

	return nBalance;
}

int64_t GetAccountBalance(const std::string& strAccount, int nMinDepth, const isminefilter& filter)
{
	CWalletDB walletdb(pwalletMain->strWalletFile);

	return GetAccountBalance(walletdb, strAccount, nMinDepth, filter);
}

json_spirit::Value getbalance(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() > 3)
	{
		throw std::runtime_error(
			"getbalance ( \"account\" minconf includeWatchonly )\n"
			"\nIf account is not specified, returns the server's total available balance.\n"
			"If account is specified, returns the balance in the account.\n"
			"Note that the account \"\" is not the same as leaving the parameter out.\n"
			"The server total may be different to the balance in the default \"\" account.\n"
			"\nArguments:\n"
			"1. \"account\"      (string, optional) The selected account, or \"*\" for entire wallet. It may be the default account using \"\".\n"
			"2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
			"3. includeWatchonly (bool, optional, default=false) Also include balance in watchonly addresses (see 'importaddress')\n"
			"\nResult:\n"
			"amount              (numeric) The total amount in XDN received for this account.\n"
			"\nExamples:\n"
			"\nThe total amount in the server across all accounts\n"
			+ HelpExampleCli("getbalance", "") +
			"\nThe total amount in the server across all accounts, with at least 5 confirmations\n"
			+ HelpExampleCli("getbalance", "\"*\" 6") +
			"\nThe total amount in the default account with at least 1 confirmation\n"
			+ HelpExampleCli("getbalance", "\"\"") +
			"\nThe total amount in the account named tabby with at least 10 confirmations\n"
			+ HelpExampleCli("getbalance", "\"tabby\" 10") +
			"\nAs a json rpc call\n"
			+ HelpExampleRpc("getbalance", "\"tabby\", 10")
		);
	}

	if (params.size() == 0)
	{
		return  ValueFromAmount(pwalletMain->GetBalance());
	}

	int nMinDepth = 1;
	if (params.size() > 1)
	{
		nMinDepth = params[1].get_int();
	}

	isminefilter filter = ISMINE_SPENDABLE;

	if(params.size() > 2 && params[2].get_bool())
	{
		filter = filter | ISMINE_WATCH_ONLY;
	}

	if (params[0].get_str() == "*")
	{
		// Calculate total balance a different way from GetBalance()
		// (GetBalance() sums up all unspent TxOuts)
		// getbalance and getbalance '*' 0 should return the same number.
		CAmount nBalance = 0;
		for (mapWallet_t::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
		{
			const CWalletTx& wtx = (*it).second;
			if (!IsFinalTx(wtx) || wtx.GetBlocksToMaturity() > 0 || wtx.GetDepthInMainChain() < 0)
			{
				continue;
			}
			
			CAmount allFee;
			std::string strSentAccount;
			std::list<std::pair<CTxDestination, int64_t> > listReceived;
			std::list<std::pair<CTxDestination, int64_t> > listSent;
			
			wtx.GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);
			
			if (wtx.GetDepthInMainChain() >= nMinDepth)
			{
				for(const std::pair<CTxDestination, int64_t>& r : listReceived)
				{
					nBalance += r.second;
				}
			}
			
			for(const std::pair<CTxDestination, int64_t>& r : listSent)
			{
				nBalance -= r.second;
			}
			
			nBalance -= allFee;
		}
		
		return  ValueFromAmount(nBalance);
	}

	accountingDeprecationCheck();

	std::string strAccount = AccountFromValue(params[0]);

	CAmount nBalance = GetAccountBalance(strAccount, nMinDepth, filter);

	return ValueFromAmount(nBalance);
}

json_spirit::Value movecmd(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 3 || params.size() > 5)
	{
		throw std::runtime_error(
			"move \"fromaccount\" \"toaccount\" amount ( minconf \"comment\" )\n"
			"\nMove a specified amount from one account in your wallet to another.\n"
			"\nArguments:\n"
			"1. \"fromaccount\"   (string, required) The name of the account to move funds from. May be the default account using \"\".\n"
			"2. \"toaccount\"     (string, required) The name of the account to move funds to. May be the default account using \"\".\n"
			"3. minconf           (numeric, optional, default=1) Only use funds with at least this many confirmations.\n"
			"4. \"comment\"       (string, optional) An optional comment, stored in the wallet only.\n"
			"\nResult:\n"
			"true|false           (boolean) true if successfull.\n"
			"\nExamples:\n"
			"\nMove 0.01 XDN from the default account to the account named tabby\n"
			+ HelpExampleCli("move", "\"\" \"tabby\" 0.01") +
			"\nMove 0.01 XDN timotei to akiko with a comment and funds have 10 confirmations\n"
			+ HelpExampleCli("move", "\"timotei\" \"akiko\" 0.01 10 \"happy birthday!\"") +
			"\nAs a json XDN call\n"
			+ HelpExampleRpc("move", "\"timotei\", \"akiko\", 0.01, 10, \"happy birthday!\"")
		);
	}

	accountingDeprecationCheck();

	std::string strFrom = AccountFromValue(params[0]);
	std::string strTo = AccountFromValue(params[1]);
	CAmount nAmount = AmountFromValue(params[2]);

	if (params.size() > 3)
	{
		// unused parameter, used to be nMinDepth, keep type-checking it though
		(void)params[3].get_int();
	}

	std::string strComment;
	if (params.size() > 4)
	{
		strComment = params[4].get_str();
	}

	CWalletDB walletdb(pwalletMain->strWalletFile);

	if (!walletdb.TxnBegin())
	{
		throw JSONRPCError(RPC_DATABASE_ERROR, "database error");
	}

	int64_t nNow = GetAdjustedTime();

	// Debit
	CAccountingEntry debit;

	debit.nOrderPos = pwalletMain->IncOrderPosNext(&walletdb);
	debit.strAccount = strFrom;
	debit.nCreditDebit = -nAmount;
	debit.nTime = nNow;
	debit.strOtherAccount = strTo;
	debit.strComment = strComment;

	pwalletMain->AddAccountingEntry(debit, walletdb);

	// Credit
	CAccountingEntry credit;

	credit.nOrderPos = pwalletMain->IncOrderPosNext(&walletdb);
	credit.strAccount = strTo;
	credit.nCreditDebit = nAmount;
	credit.nTime = nNow;
	credit.strOtherAccount = strFrom;
	credit.strComment = strComment;

	pwalletMain->AddAccountingEntry(credit, walletdb);

	if (!walletdb.TxnCommit())
	{
		throw JSONRPCError(RPC_DATABASE_ERROR, "database error");
	}

	return true;
}

json_spirit::Value sendfrom(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 3 || params.size() > 7)
	{
		throw std::runtime_error(
			"sendfrom \"fromaccount\" \"toDigitalNoteaddress\" amount ( minconf \"comment\" \"comment-to\" )\n"
			"\nSent an amount from an account to a DigitalNote address.\n"
			"The amount is a real and is rounded to the nearest 0.00000001."
			+ HelpRequiringPassphrase() + "\n"
			"\nArguments:\n"
			"1. \"fromaccount\"       (string, required) The name of the account to send funds from. May be the default account using \"\".\n"
			"2. \"toDigitalNoteaddress\"  (string, required) The DigitalNote address to send funds to.\n"
			"3. amount                (numeric, required) The amount in IC. (transaction fee is added on top).\n"
			"4. minconf               (numeric, optional, default=1) Only use funds with at least this many confirmations.\n"
			"5. \"comment\"           (string, optional) A comment used to store what the transaction is for. \n"
			"                                     This is not part of the transaction, just kept in your wallet.\n"
			"6. \"comment-to\"        (string, optional) An optional comment to store the name of the person or organization \n"
			"                                     to which you're sending the transaction. This is not part of the transaction, \n"
			"                                     it is just kept in your wallet.\n"
			"\nResult:\n"
			"\"transactionid\"        (string) The transaction id.\n"
			"\nExamples:\n"
			"\nSend 0.01 XDN from the default account to the address, must have at least 1 confirmation\n"
			+ HelpExampleCli("sendfrom", "\"\" \"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" 0.01") +
			"\nSend 0.01 from the tabby account to the given address, funds must have at least 10 confirmations\n"
			+ HelpExampleCli("sendfrom", "\"tabby\" \"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" 0.01 10 \"donation\" \"seans outpost\"") +
			"\nAs a json rpc call\n"
			+ HelpExampleRpc("sendfrom", "\"tabby\", \"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\", 0.01, 10, \"donation\", \"seans outpost\"")
		);
	}

	EnsureWalletIsUnlocked();

	std::string strAccount = AccountFromValue(params[0]);
	CDigitalNoteAddress address(params[1].get_str());
	if (!address.IsValid())
	{
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid DigitalNote address");
	}

	CAmount nAmount = AmountFromValue(params[2]);

	int nMinDepth = 1;
	if (params.size() > 3)
	{
		nMinDepth = params[3].get_int();
	}

	CWalletTx wtx;
	wtx.strFromAccount = strAccount;

	if (params.size() > 4 && params[4].type() != json_spirit::null_type && !params[4].get_str().empty())
	{
		wtx.mapValue["comment"] = params[4].get_str();
	}

	if (params.size() > 5 && params[5].type() != json_spirit::null_type && !params[5].get_str().empty())
	{
		wtx.mapValue["to"]      = params[5].get_str();
	}

	std::string sNarr;
	if (params.size() > 6 && params[6].type() != json_spirit::null_type && !params[6].get_str().empty())
	{
		sNarr = params[6].get_str();
	}

	if (sNarr.length() > 24)
	{
		throw std::runtime_error("Narration must be 24 characters or less.");
	}

	// Check funds
	int64_t nBalance = GetAccountBalance(strAccount, nMinDepth, ISMINE_SPENDABLE);
	if (nAmount > nBalance)
	{
		throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");
	}

	// Send
	std::string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, sNarr, wtx);
	if (strError != "")
	{
		throw JSONRPCError(RPC_WALLET_ERROR, strError);
	}

	return wtx.GetHash().GetHex();
}

json_spirit::Value sendmany(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 2 || params.size() > 4)
	{
		throw std::runtime_error(
			"sendmany \"fromaccount\" {\"address\":amount,...} ( minconf \"comment\" )\n"
			"\nSend multiple times. Amounts are double-precision floating point numbers."
			+ HelpRequiringPassphrase() + "\n"
			"\nArguments:\n"
			"1. \"fromaccount\"         (string, required) The account to send the funds from, can be \"\" for the default account\n"
			"2. \"amounts\"             (string, required) A json object with addresses and amounts\n"
			"    {\n"
			"      \"address\":amount   (numeric) The DigitalNote address is the key, the numeric amount in XDN is the value\n"
			"      ,...\n"
			"    }\n"
			"3. minconf                 (numeric, optional, default=1) Only use the balance confirmed at least this many times.\n"
			"4. \"comment\"             (string, optional) A comment\n"
			"\nResult:\n"
			"\"transactionid\"          (string) The transaction id for the send. Only 1 transaction is created regardless of \n"
			"                                    the number of addresses.\n"
			"\nExamples:\n"
			"\nSend two amounts to two different addresses:\n"
			+ HelpExampleCli("sendmany", "\"tabby\" \"{\\\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\\\":0.01,\\\"ThiLpx7oYd5YuuhsJAUD5ZsEX2YHgU98Us\\\":0.02}\"") +
			"\nSend two amounts to two different addresses setting the confirmation and comment:\n"
			+ HelpExampleCli("sendmany", "\"tabby\" \"{\\\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\\\":0.01,\\\"ThiLpx7oYd5YuuhsJAUD5ZsEX2YHgU98Us\\\":0.02}\" 10 \"testing\"") +
			"\nAs a json rpc call\n"
			+ HelpExampleRpc("sendmany", "\"tabby\", \"{\\\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\\\":0.01,\\\"ThiLpx7oYd5YuuhsJAUD5ZsEX2YHgU98Us\\\":0.02}\", 10, \"testing\"")
		);
	}

	std::string strAccount = AccountFromValue(params[0]);
	json_spirit::Object sendTo = params[1].get_obj();
	int nMinDepth = 1;

	if (params.size() > 2)
	{
		nMinDepth = params[2].get_int();
	}

	CWalletTx wtx;
	wtx.strFromAccount = strAccount;

	if (params.size() > 3 && params[3].type() != json_spirit::null_type && !params[3].get_str().empty())
	{
		wtx.mapValue["comment"] = params[3].get_str();
	}

	std::set<CDigitalNoteAddress> setAddress;
	std::vector<std::pair<CScript, int64_t> > vecSend;

	int64_t totalAmount = 0;
	for(const json_spirit::Pair& s : sendTo)
	{
		CDigitalNoteAddress address(s.name_);
		if (!address.IsValid())
		{
			throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid DigitalNote address: ")+s.name_);
		}
		
		if (setAddress.count(address))
		{
			throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ")+s.name_);
		}
		
		setAddress.insert(address);

		CScript scriptPubKey;
		scriptPubKey.SetDestination(address.Get());
		CAmount nAmount = AmountFromValue(s.value_);

		totalAmount += nAmount;

		vecSend.push_back(std::make_pair(scriptPubKey, nAmount));
	}

	EnsureWalletIsUnlocked();

	// Check funds
	int64_t nBalance = GetAccountBalance(strAccount, nMinDepth, ISMINE_SPENDABLE);
	if (totalAmount > nBalance)
	{
		throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");
	}

	// Send
	CReserveKey keyChange(pwalletMain);
	int64_t nFeeRequired = 0;
	int nChangePos;
	std::string strFailReason;
	bool fCreated = pwalletMain->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired, nChangePos, strFailReason);

	if (!fCreated)
	{
		if (totalAmount + nFeeRequired > pwalletMain->GetBalance())
		{
			throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
		}
		
		throw JSONRPCError(RPC_WALLET_ERROR, "Transaction creation failed");
	}

	if (!pwalletMain->CommitTransaction(wtx, keyChange))
	{
		throw JSONRPCError(RPC_WALLET_ERROR, "Transaction commit failed");
	}

	return wtx.GetHash().GetHex();
}

json_spirit::Value addmultisigaddress(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 2 || params.size() > 3)
	{
		throw std::runtime_error(
			"addmultisigaddress nrequired [\"key\",...] ( \"account\" )\n"
			"\nAdd a nrequired-to-sign multisignature address to the wallet.\n"
			"Each key is a DigitalNote address or hex-encoded public key.\n"
			"If 'account' is specified, assign address to that account.\n"

			"\nArguments:\n"
			"1. nrequired        (numeric, required) The number of required signatures out of the n keys or addresses.\n"
			"2. \"keysobject\"   (string, required) A json array of DigitalNote addresses or hex-encoded public keys\n"
			"     [\n"
			"       \"address\"  (string) DigitalNote address or hex-encoded public key\n"
			"       ...,\n"
			"     ]\n"
			"3. \"account\"      (string, optional) An account to assign the addresses to.\n"

			"\nResult:\n"
			"\"DigitalNote\"  (string) A DigitalNote address associated with the keys.\n"

			"\nExamples:\n"
			"\nAdd a multisig address from 2 addresses\n"
			+ HelpExampleCli("addmultisigaddress", "2 \"[\\\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\\\",\\\"ThiLpx7oYd5YuuhsJAUD5ZsEX2YHgU98Us\\\"]\"") +
			"\nAs json rpc call\n"
			+ HelpExampleRpc("addmultisigaddress", "2, \"[\\\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\\\",\\\"ThiLpx7oYd5YuuhsJAUD5ZsEX2YHgU98Us\\\"]\"")
		);
	}

	int nRequired = params[0].get_int();
	const json_spirit::Array& keys = params[1].get_array();
	std::string strAccount;

	if (params.size() > 2)
	{
		strAccount = AccountFromValue(params[2]);
	}

	// Gather public keys
	if (nRequired < 1)
	{
		throw std::runtime_error("a multisignature address must require at least one key to redeem");
	}

	if ((int)keys.size() < nRequired)
	{
		throw std::runtime_error(
			strprintf(
				"not enough keys supplied (got %u keys, but need at least %d to redeem)",
				keys.size(),
				nRequired
			)
		);
	}

	std::vector<CPubKey> pubkeys;
	pubkeys.resize(keys.size());

	for (unsigned int i = 0; i < keys.size(); i++)
	{
		const std::string& ks = keys[i].get_str();

		// Case 1: DigitalNote address and we have full public key:
		CDigitalNoteAddress address(ks);
		if (pwalletMain && address.IsValid())
		{
			CKeyID keyID;
			CPubKey vchPubKey;
			
			if (!address.GetKeyID(keyID))
			{
				throw std::runtime_error(strprintf("%s does not refer to a key",ks));
			}
			
			if (!pwalletMain->GetPubKey(keyID, vchPubKey))
			{
				throw std::runtime_error(strprintf("no full public key for address %s",ks));
			}
			if (!vchPubKey.IsFullyValid())
			{
				throw std::runtime_error(" Invalid public key: "+ks);
			}
			
			pubkeys[i] = vchPubKey;
		}
		// Case 2: hex public key
		else if (IsHex(ks))
		{
			CPubKey vchPubKey(ParseHex(ks));
			
			if (!vchPubKey.IsFullyValid())
			{
				throw std::runtime_error(" Invalid public key: "+ks);
			}
			
			pubkeys[i] = vchPubKey;
		}
		else
		{
			throw std::runtime_error(" Invalid public key: "+ks);
		}
	}

	// Construct using pay-to-script-hash:
	CScript inner;
	inner.SetMultisig(nRequired, pubkeys);
	CScriptID innerID = inner.GetID();

	if (!pwalletMain->AddCScript(inner))
	{
		throw std::runtime_error("AddCScript() failed");
	}

	pwalletMain->SetAddressBookName(innerID, strAccount);

	return CDigitalNoteAddress(innerID).ToString();
}

json_spirit::Value addredeemscript(const json_spirit::Array& params, bool fHelp)
{
	if (fHelp || params.size() < 1 || params.size() > 2)
	{
		throw std::runtime_error("addredeemscript <redeemScript> [account]\n"
			"Add a P2SH address with a specified redeemScript to the wallet.\n"
			"If [account] is specified, assign address to [account]."
		);
	}

	std::string strAccount;
	if (params.size() > 1)
	{
		strAccount = AccountFromValue(params[1]);
	}

	// Construct using pay-to-script-hash:
	std::vector<unsigned char> innerData = ParseHexV(params[0], "redeemScript");
	CScript inner(innerData.begin(), innerData.end());
	CScriptID innerID = inner.GetID();

	if (!pwalletMain->AddCScript(inner))
	{
		throw std::runtime_error("AddCScript() failed");
	}

	pwalletMain->SetAddressBookName(innerID, strAccount);

	return CDigitalNoteAddress(innerID).ToString();
}

json_spirit::Value ListReceived(const json_spirit::Array& params, bool fByAccounts)
{
	// Minimum confirmations
	int nMinDepth = 1;
	if (params.size() > 0)
	{
		nMinDepth = params[0].get_int();
	}

	// Whether to include empty accounts
	bool fIncludeEmpty = false;
	if (params.size() > 1)
	{
		fIncludeEmpty = params[1].get_bool();
	}

	isminefilter filter = ISMINE_SPENDABLE;

	if(params.size() > 2 && params[2].get_bool())
	{
		filter = filter | ISMINE_WATCH_ONLY;
	}

	// Tally
	std::map<CDigitalNoteAddress, tallyitem> mapTally;
	for (mapWallet_t::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
	{
		const CWalletTx& wtx = (*it).second;

		if (wtx.IsCoinBase() || wtx.IsCoinStake() || !IsFinalTx(wtx))
		{
			continue;
		}
		
		int nDepth = wtx.GetDepthInMainChain();
		int nBCDepth = wtx.GetDepthInMainChain(false);
		if (nDepth < nMinDepth)
		{
			continue;
		}
		
		for(const CTxOut& txout : wtx.vout)
		{
			CTxDestination address;
			
			if (!ExtractDestination(txout.scriptPubKey, address))
			{
				continue;
			}
			
			isminefilter mine = IsMine(*pwalletMain, address);
			
			if(!(mine & filter))
			{
				continue;
			}
			
			tallyitem& item = mapTally[address];
			
			item.nAmount += txout.nValue;
			item.nConf = std::min(item.nConf, nDepth);
			item.nBCConf = std::min(item.nBCConf, nBCDepth);
			item.txids.push_back(wtx.GetHash());
			
			if (mine & ISMINE_WATCH_ONLY)
			{
				item.fIsWatchonly = true;
			}
		}
	}

	// Reply
	json_spirit::Array ret;
	std::map<std::string, tallyitem> mapAccountTally;

	for(const std::pair<CDigitalNoteAddress, std::string>& item : pwalletMain->mapAddressBook)
	{
		const CDigitalNoteAddress& address = item.first;
		const std::string& strAccount = item.second;
		std::map<CDigitalNoteAddress, tallyitem>::iterator it = mapTally.find(address);
		
		if (it == mapTally.end() && !fIncludeEmpty)
		{
			continue;
		}
		
		CAmount nAmount = 0;
		int nConf = std::numeric_limits<int>::max();
		int nBCConf = std::numeric_limits<int>::max();
		bool fIsWatchonly = false;
		
		if (it != mapTally.end())
		{
			nAmount = (*it).second.nAmount;
			nConf = (*it).second.nConf;
			nBCConf = (*it).second.nBCConf;
			fIsWatchonly = (*it).second.fIsWatchonly;
		}

		if (fByAccounts)
		{
			tallyitem& item = mapAccountTally[strAccount];
			
			item.nAmount += nAmount;
			item.nConf = std::min(item.nConf, nConf);
			item.nBCConf = std::min(item.nBCConf, nBCConf);
			item.fIsWatchonly = fIsWatchonly;
		}
		else
		{
			json_spirit::Object obj;
			
			if(fIsWatchonly)
			{
				obj.push_back(json_spirit::Pair("involvesWatchonly", true));
			}
			
			obj.push_back(json_spirit::Pair("address",       address.ToString()));
			obj.push_back(json_spirit::Pair("account",       strAccount));
			obj.push_back(json_spirit::Pair("amount",        ValueFromAmount(nAmount)));
			obj.push_back(json_spirit::Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
			obj.push_back(json_spirit::Pair("bcconfirmations", (nBCConf == std::numeric_limits<int>::max() ? 0 : nBCConf)));
			
			json_spirit::Array transactions;
			if (it != mapTally.end())
			{
				for(const uint256& item : (*it).second.txids)
				{
					transactions.push_back(item.GetHex());
				}
			}
			
			obj.push_back(json_spirit::Pair("txids", transactions));
			
			ret.push_back(obj);
		}
	}

	if (fByAccounts)
	{
		for (std::map<std::string, tallyitem>::iterator it = mapAccountTally.begin(); it != mapAccountTally.end(); ++it)
		{
			CAmount nAmount = (*it).second.nAmount;
			int nConf = (*it).second.nConf;
			int nBCConf = (*it).second.nBCConf;
			json_spirit::Object obj;
			
			if((*it).second.fIsWatchonly)
			{
				obj.push_back(json_spirit::Pair("involvesWatchonly", true));
			}
			
			obj.push_back(json_spirit::Pair("account",       (*it).first));
			obj.push_back(json_spirit::Pair("amount",        ValueFromAmount(nAmount)));
			obj.push_back(json_spirit::Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
			obj.push_back(json_spirit::Pair("bcconfirmations", (nBCConf == std::numeric_limits<int>::max() ? 0 : nBCConf)));
			
			ret.push_back(obj);
		}
	}

	return ret;
}

json_spirit::Value listreceivedbyaddress(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
	{
        throw std::runtime_error(
            "listreceivedbyaddress ( minconf includeempty includeWatchonly)\n"
            "\nList balances by receiving address.\n"
            "\nArguments:\n"
            "1. minconf       (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. includeempty  (numeric, optional, default=false) Whether to include addresses that haven't received any payments.\n"
            "3. includeWatchonly (bool, optional, default=false) Whether to include watchonly addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : \"true\",    (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"address\" : \"receivingaddress\",  (string) The receiving address\n"
            "    \"account\" : \"accountname\",       (string) The account of the receiving address. The default account is \"\".\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in XDN received by the address\n"
            "    \"confirmations\" : n                (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"bcconfirmations\" : n              (numeric) The number of Blockchain confirmations of the most recent transaction included\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("listreceivedbyaddress", "")
            + HelpExampleCli("listreceivedbyaddress", "10 true")
            + HelpExampleRpc("listreceivedbyaddress", "10, true, true")
        );
	}

    return ListReceived(params, false);
}

json_spirit::Value listreceivedbyaccount(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
	{
        throw std::runtime_error(
            "listreceivedbyaccount ( minconf includeempty includeWatchonly)\n"
            "\nList balances by account.\n"
            "\nArguments:\n"
            "1. minconf      (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. includeempty (boolean, optional, default=false) Whether to include accounts that haven't received any payments.\n"
            "3. includeWatchonly (bool, optional, default=false) Whether to include watchonly addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : \"true\",    (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"account\" : \"accountname\",  (string) The account name of the receiving account\n"
            "    \"amount\" : x.xxx,             (numeric) The total amount received by addresses with this account\n"
            "    \"confirmations\" : n           (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"bcconfirmations\" : n         (numeric) The number of Blockchain confirmations of the most recent transaction included\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("listreceivedbyaccount", "")
            + HelpExampleCli("listreceivedbyaccount", "10 true")
            + HelpExampleRpc("listreceivedbyaccount", "10, true, true")
        );
	}

    accountingDeprecationCheck();

    return ListReceived(params, true);
}

static void MaybePushAddress(json_spirit::Object & entry, const CTxDestination &dest)
{
    CDigitalNoteAddress addr;
    
	if (addr.Set(dest))
	{
        entry.push_back(json_spirit::Pair("address", addr.ToString()));
	}
}

void ListTransactions(const CWalletTx& wtx, const std::string& strAccount, int nMinDepth, bool fLong, json_spirit::Array& ret, const isminefilter& filter)
{
    CAmount nFee;
    std::string strSentAccount;
    std::list<std::pair<CTxDestination, int64_t> > listReceived;
    std::list<std::pair<CTxDestination, int64_t> > listSent;

    wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, filter);

    bool fAllAccounts = (strAccount == std::string("*"));
    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if ((!wtx.IsCoinStake()) && (!listSent.empty() || nFee != 0) && (fAllAccounts || strAccount == strSentAccount))
    {
        for(const std::pair<CTxDestination, int64_t>& s : listSent)
        {
            json_spirit::Object entry;
			
            if(involvesWatchonly || (::IsMine(*pwalletMain, s.first) & ISMINE_WATCH_ONLY))
			{
                entry.push_back(json_spirit::Pair("involvesWatchonly", true));
			}
			
            entry.push_back(json_spirit::Pair("account", strSentAccount));
            MaybePushAddress(entry, s.first);
            entry.push_back(json_spirit::Pair("category", "send"));
            entry.push_back(json_spirit::Pair("amount", ValueFromAmount(-s.second)));
            entry.push_back(json_spirit::Pair("fee", ValueFromAmount(-nFee)));
            
			if (fLong) // TODO: reference this again
			{
                WalletTxToJSON(wtx, entry);
			}
			
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth)
    {
        bool stop = false;
		
        for(const std::pair<CTxDestination, int64_t>& r : listReceived)
        {
            std::string account;
			
            if (pwalletMain->mapAddressBook.count(r.first))
			{
                account = pwalletMain->mapAddressBook[r.first];
            }
			
			if (fAllAccounts || (account == strAccount))
            {
                json_spirit::Object entry;
				
                if(involvesWatchonly || (::IsMine(*pwalletMain, r.first) & ISMINE_WATCH_ONLY))
				{
                    entry.push_back(json_spirit::Pair("involvesWatchonly", true));
                }
				
				entry.push_back(json_spirit::Pair("account", account));
                MaybePushAddress(entry, r.first);
                
				if (wtx.IsCoinBase() || wtx.IsCoinStake())
                {
                    if (wtx.GetDepthInMainChain() < 1)
					{
                        entry.push_back(json_spirit::Pair("category", "orphan"));
					}
                    else if (wtx.GetBlocksToMaturity() > 0)
					{
                        entry.push_back(json_spirit::Pair("category", "immature"));
					}
                    else
					{
                        entry.push_back(json_spirit::Pair("category", "generate"));
					}
                }
                else
                {
                    entry.push_back(json_spirit::Pair("category", "receive"));
                }
				
                if (!wtx.IsCoinStake())
				{
                    entry.push_back(json_spirit::Pair("amount", ValueFromAmount(r.second)));
				}
                else
                {
                    entry.push_back(json_spirit::Pair("amount", ValueFromAmount(-nFee)));
                    stop = true; // only one coinstake output
                }
                
				if (fLong) // TODO: reference this again
				{
                    WalletTxToJSON(wtx, entry);
				}
				
                ret.push_back(entry);
            }
			
            if (stop)
			{
                break;
			}
        }
    }
}

void AcentryToJSON(const CAccountingEntry& acentry, const std::string& strAccount, json_spirit::Array& ret)
{
    bool fAllAccounts = (strAccount == std::string("*"));

    if (fAllAccounts || acentry.strAccount == strAccount)
    {
        json_spirit::Object entry;
        
		entry.push_back(json_spirit::Pair("account", acentry.strAccount));
        entry.push_back(json_spirit::Pair("category", "move"));
        entry.push_back(json_spirit::Pair("time", (int64_t)acentry.nTime));
        entry.push_back(json_spirit::Pair("amount", ValueFromAmount(acentry.nCreditDebit)));
        entry.push_back(json_spirit::Pair("otheraccount", acentry.strOtherAccount));
        entry.push_back(json_spirit::Pair("comment", acentry.strComment));
        
		ret.push_back(entry);
    }
}

json_spirit::Value listtransactions(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 4)
	{
        throw std::runtime_error(
            "listtransactions ( \"account\" count from includeWatchonly)\n"
            "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions for account 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"    (string, optional) The account name. If not included, it will list all transactions for all accounts.\n"
            "                                     If \"\" is set, it will list transactions for the default account.\n"
            "2. count          (numeric, optional, default=10) The number of transactions to return\n"
            "3. from           (numeric, optional, default=0) The number of transactions to skip\n"
            "4. includeWatchonly (bool, optional, default=false) Include transactions to watchonly addresses (see 'importaddress')\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"account\":\"accountname\",       (string) The account name associated with the transaction. \n"
            "                                                It will be \"\" for the default account.\n"
            "    \"address\":\"DigitalNote\",    (string) The DigitalNote address of the transaction. Not present for \n"
            "                                                move transactions (category = move).\n"
            "    \"category\":\"send|receive|move\", (string) The transaction category. 'move' is a local (off blockchain)\n"
            "                                                transaction between accounts, and not associated with an address,\n"
            "                                                transaction id or block. 'send' and 'receive' transactions are \n"
            "                                                associated with an address, transaction id and block details\n"
            "    \"amount\": x.xxx,          (numeric) The amount in TX. This is negative for the 'send' category, and for the\n"
            "                                         'move' category for moves outbound. It is positive for the 'receive' category,\n"
            "                                         and for the 'move' category for inbound funds.\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in XDN. This is negative and only available for the \n"
            "                                         'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and \n"
            "                                         'receive' category of transactions.\n"
            "    \"bcconfirmations\": n,     (numeric) The number of Blcokchain confirmations for the transaction. Available for 'send'\n"
            "                                          and 'receive' category of transactions.\n"
            "    \"blockhash\": \"hashvalue\", (string) The block hash containing the transaction. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The block index containing the transaction. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"txid\": \"transactionid\", (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (midnight Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (midnight Jan 1 1970 GMT). Available \n"
            "                                          for 'send' and 'receive' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"otheraccount\": \"accountname\",  (string) For the 'move' category of transactions, the account the funds came \n"
            "                                          from (for receiving funds, positive amounts), or went to (for sending funds,\n"
            "                                          negative amounts).\n"
            "  }\n"
            "]\n"

            "\nExamples:\n"
            "\nList the most recent 10 transactions in the systems\n"
            + HelpExampleCli("listtransactions", "") +
            "\nList the most recent 10 transactions for the tabby account\n"
            + HelpExampleCli("listtransactions", "\"tabby\"") +
            "\nList transactions 100 to 120 from the tabby account\n"
            + HelpExampleCli("listtransactions", "\"tabby\" 20 100") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("listtransactions", "\"tabby\", 20, 100")
        );
	}

    std::string strAccount = "*";
    if (params.size() > 0)
	{
        strAccount = params[0].get_str();
    }
	
	int nCount = 10;
    if (params.size() > 1)
	{
        nCount = params[1].get_int();
    }
	
	int nFrom = 0;
    if (params.size() > 2)
	{
        nFrom = params[2].get_int();
    }
	
	isminefilter filter = ISMINE_SPENDABLE;
    
	if(params.size() > 3 && params[3].get_bool())
	{
		filter = filter | ISMINE_WATCH_ONLY;
	}
	
    if (nCount < 0)
	{
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    }
	
	if (nFrom < 0)
	{
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");
	}
	
    json_spirit::Array ret;

    const TxItems & txOrdered = pwalletMain->wtxOrdered;

    // iterate backwards until we have nCount items to return:
    for (TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
    {
        CWalletTx *const pwtx = (*it).second.first;
        if (pwtx != 0)
		{
            ListTransactions(*pwtx, strAccount, 0, true, ret, filter);
        }
		
		CAccountingEntry *const pacentry = (*it).second.second;
		if (pacentry != 0)
		{
            AcentryToJSON(*pacentry, strAccount, ret);
		}
		
        if ((int)ret.size() >= (nCount+nFrom))
		{
			break;
		}
    }
	
    // ret is newest to oldest
    if (nFrom > (int)ret.size())
	{
        nFrom = ret.size();
    }
	
	if ((nFrom + nCount) > (int)ret.size())
	{
        nCount = ret.size() - nFrom;
    }
	
	json_spirit::Array::iterator first = ret.begin();
    std::advance(first, nFrom);
    json_spirit::Array::iterator last = ret.begin();
    std::advance(last, nFrom+nCount);

    if (last != ret.end())
	{
		ret.erase(last, ret.end());
	}
	
    if (first != ret.begin())
	{
		ret.erase(ret.begin(), first);
	}
	
    std::reverse(ret.begin(), ret.end()); // Return oldest to newest

    return ret;
}

json_spirit::Value listaccounts(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
	{
        throw std::runtime_error(
            "listaccounts ( minconf includeWatchonly)\n"
            "\nReturns json_spirit::Object that has account names as keys, account balances as values.\n"
            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=1) Only onclude transactions with at least this many confirmations\n"
            "2. includeWatchonly (bool, optional, default=false) Include balances in watchonly addresses (see 'importaddress')\n"
            "\nResult:\n"
            "{                      (json object where keys are account names, and values are numeric balances\n"
            "  \"account\": x.xxx,  (numeric) The property name is the account name, and the value is the total balance for the account.\n"
            "  ...\n"
            "}\n"
            "\nExamples:\n"
            "\nList account balances where there at least 1 confirmation\n"
            + HelpExampleCli("listaccounts", "") +
            "\nList account balances including zero confirmation transactions\n"
            + HelpExampleCli("listaccounts", "0") +
            "\nList account balances for 10 or more confirmations\n"
            + HelpExampleCli("listaccounts", "10") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("listaccounts", "10")
        );
	}
	
    accountingDeprecationCheck();

    int nMinDepth = 1;
    if (params.size() > 0)
	{
        nMinDepth = params[0].get_int();
    }
	
	isminefilter includeWatchonly = ISMINE_SPENDABLE;
    
	if(params.size() > 1 && params[1].get_bool())
	{
		includeWatchonly = includeWatchonly | ISMINE_WATCH_ONLY;
	}
	
    std::map<std::string, CAmount> mapAccountBalances;
	
    for(const pairAddressBook_t& entry : pwalletMain->mapAddressBook)
	{
        if (IsMine(*pwalletMain, entry.first) & includeWatchonly) // This address belongs to me
		{
            mapAccountBalances[entry.second] = 0;
		}
    }

    for (mapWallet_t::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        CAmount nFee;
        std::string strSentAccount;
        std::list<std::pair<CTxDestination, int64_t> > listReceived;
        std::list<std::pair<CTxDestination, int64_t> > listSent;
        int nDepth = wtx.GetDepthInMainChain();
        
		if (nDepth < 0)
		{
            continue;
        }
		
		wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, includeWatchonly);
        mapAccountBalances[strSentAccount] -= nFee;
		
        for(const std::pair<CTxDestination, int64_t>& s : listSent)
		{
            mapAccountBalances[strSentAccount] -= s.second;
		}
		
        if (nDepth >= nMinDepth && wtx.GetBlocksToMaturity() == 0)
        {
            for(const std::pair<CTxDestination, int64_t>& r : listReceived)
			{
                if (pwalletMain->mapAddressBook.count(r.first))
				{
                    mapAccountBalances[pwalletMain->mapAddressBook[r.first]] += r.second;
				}
                else
				{
                    mapAccountBalances[""] += r.second;
				}
			}
        }
    }

    const std::list<CAccountingEntry> & acentries = pwalletMain->laccentries;
    for(const CAccountingEntry& entry : acentries)
	{
        mapAccountBalances[entry.strAccount] += entry.nCreditDebit;
	}
	
    json_spirit::Object ret;
	for(const std::pair<std::string, CAmount>& accountBalance : mapAccountBalances)
	{
        ret.push_back(json_spirit::Pair(accountBalance.first, ValueFromAmount(accountBalance.second)));
    }
	
    return ret;
}

json_spirit::Value listsinceblock(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp)
	{
       throw std::runtime_error(
            "listsinceblock ( \"blockhash\" target-confirmations includeWatchonly)\n"
            "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted\n"
            "\nArguments:\n"
            "1. \"blockhash\"   (string, optional) The block hash to list transactions since\n"
            "2. target-confirmations:    (numeric, optional) The confirmations required, must be 1 or more\n"
            "3. includeWatchonly:        (bool, optional, default=false) Include transactions to watchonly addresses (see 'importaddress')"
            "\nResult:\n"
            "{\n"
            "  \"transactions\": [\n"
            "    \"account\":\"accountname\",       (string) The account name associated with the transaction. Will be \"\" for the default account.\n"
            "    \"address\":\"DigitalNote\",    (string) The DigitalNote address of the transaction. Not present for move transactions (category = move).\n"
            "    \"category\":\"send|receive\",     (string) The transaction category. 'send' has negative amounts, 'receive' has positive amounts.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in TX. This is negative for the 'send' category, and for the 'move' category for moves \n"
            "                                          outbound. It is positive for the 'receive' category, and for the 'move' category for inbound funds.\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in XDN. This is negative and only available for the 'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"bcconfirmations\" : n,    (numeric) The number of Blockchain confirmations for the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blockhash\": \"hashvalue\",     (string) The block hash containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The block index containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\",  (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (Jan 1 1970 GMT). Available for 'send' and 'receive' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"to\": \"...\",            (string) If a comment to is associated with the transaction.\n"
             "  ],\n"
            "  \"lastblock\": \"lastblockhash\"     (string) The hash of the last block\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("listsinceblock", "")
            + HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 10")
            + HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 10")
        );
	}

    CBlockIndex *pindex = NULL;
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    if (params.size() > 0)
    {
        uint256 blockId = 0;

        blockId.SetHex(params[0].get_str());
        pindex = CBlockLocator(blockId).GetBlockIndex();
    }

    if (params.size() > 1)
    {
        target_confirms = params[1].get_int();

        if (target_confirms < 1)
		{
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
		}
    }

    if(params.size() > 2 && params[2].get_bool())
	{
		filter = filter | ISMINE_WATCH_ONLY;
	}
	
    int depth = pindex ? (1 + nBestHeight - pindex->nHeight) : -1;

    json_spirit::Array transactions;

    for (mapWallet_t::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); it++)
    {
        CWalletTx tx = (*it).second;

        if (depth == -1 || tx.GetDepthInMainChain(false) < depth)
		{
            ListTransactions(tx, "*", 0, true, transactions, filter);
		}
    }

    uint256 lastblock;

    if (target_confirms == 1)
    {
        lastblock = hashBestChain;
    }
    else
    {
        int target_height = pindexBest->nHeight + 1 - target_confirms;

        CBlockIndex *block;
        for (block = pindexBest; block && block->nHeight > target_height; block = block->pprev) 
		{
			
		}

        lastblock = block ? block->GetBlockHash() : 0;
    }

    json_spirit::Object ret;
    ret.push_back(json_spirit::Pair("transactions", transactions));
    ret.push_back(json_spirit::Pair("lastblock", lastblock.GetHex()));

    return ret;
}

json_spirit::Value gettransaction(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
	{
        throw std::runtime_error(
            "gettransaction \"txid\" ( includeWatchonly )\n"
            "\nGet detailed information about in-wallet transaction <txid>\n"
            "\nArguments:\n"
            "1. \"txid\"    (string, required) The transaction id\n"
            "2. \"includeWatchonly\"    (bool, optional, default=false) Whether to include watchonly addresses in balance calculation and details[]\n"
            "\nResult:\n"
            "{\n"
            "  \"amount\" : x.xxx,        (numeric) The transaction amount in XDN\n"
            "  \"confirmations\" : n,     (numeric) The number of confirmations\n"
            "  \"bcconfirmations\" : n,   (numeric) The number of Blockchain confirmations\n"
            "  \"blockhash\" : \"hash\",  (string) The block hash\n"
            "  \"blockindex\" : xx,       (numeric) The block index\n"
            "  \"blocktime\" : ttt,       (numeric) The time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"txid\" : \"transactionid\",   (string) The transaction id.\n"
            "  \"time\" : ttt,            (numeric) The transaction time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"timereceived\" : ttt,    (numeric) The time received in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"details\" : [\n"
            "    {\n"
            "      \"account\" : \"accountname\",  (string) The account name involved in the transaction, can be \"\" for the default account.\n"
            "      \"address\" : \"DigitalNote\",   (string) The DigitalNote address involved in the transaction\n"
            "      \"category\" : \"send|receive\",    (string) The category, either 'send' or 'receive'\n"
            "      \"amount\" : x.xxx                  (numeric) The amount in XDN\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"hex\" : \"data\"         (string) Raw data for transaction\n"
            "}\n"

            "\nbExamples\n"
            + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
        );
	}
	
    uint256 hash;
    hash.SetHex(params[0].get_str());

    isminefilter filter = ISMINE_SPENDABLE;
    
	if(params.size() > 1 && params[1].get_bool())
	{
		filter = filter | ISMINE_WATCH_ONLY;
	}
	
    json_spirit::Object entry;

    if (pwalletMain->mapWallet.count(hash))
    {
        const CWalletTx& wtx = pwalletMain->mapWallet[hash];

        TxToJSON(wtx, 0, entry);

        int64_t nCredit = wtx.GetCredit(filter);
        int64_t nDebit = wtx.GetDebit(filter);
        int64_t nNet = nCredit - nDebit;
        int64_t nFee = (wtx.IsFromMe(filter) ? wtx.GetValueOut() - nDebit : 0);

        entry.push_back(json_spirit::Pair("amount", ValueFromAmount(nNet - nFee)));
        
		if (wtx.IsFromMe(filter))
		{
            entry.push_back(json_spirit::Pair("fee", ValueFromAmount(nFee)));
		}
		
        WalletTxToJSON(wtx, entry);

        json_spirit::Array details;
        
		ListTransactions(pwalletMain->mapWallet[hash], "*", 0, false, details, filter);
        
		entry.push_back(json_spirit::Pair("details", details));
    }
    else
    {
        CTransaction tx;
        uint256 hashBlock = 0;
        
		if (GetTransaction(hash, tx, hashBlock))
        {
            TxToJSON(tx, 0, entry);
            
			if (hashBlock == 0)
			{
                entry.push_back(json_spirit::Pair("confirmations", 0));
			}
            else
            {
                entry.push_back(json_spirit::Pair("blockhash", hashBlock.GetHex()));
                std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
                
				if (mi != mapBlockIndex.end() && (*mi).second)
                {
                    CBlockIndex* pindex = (*mi).second;
                    
					if (pindex->IsInMainChain())
					{
                        entry.push_back(json_spirit::Pair("confirmations", 1 + nBestHeight - pindex->nHeight));
					}
                    else
					{
                        entry.push_back(json_spirit::Pair("confirmations", 0));
					}
                }
            }
        }
        else
		{
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available about transaction");
		}
    }

    return entry;
}

json_spirit::Value backupwallet(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
	{
        throw std::runtime_error(
            "backupwallet \"destination\"\n"
            "\nSafely copies wallet.dat to destination, which can be a directory or a path with filename.\n"
            "\nArguments:\n"
            "1. \"destination\"   (string) The destination directory or file\n"
            "\nExamples:\n"
            + HelpExampleCli("backupwallet", "\"backup.dat\"")
            + HelpExampleRpc("backupwallet", "\"backup.dat\"")
        );
	}
	
    std::string strDest = params[0].get_str();
    
	if (!BackupWallet(*pwalletMain, strDest))
	{
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");
	}
	
    return json_spirit::Value::null;
}

json_spirit::Value keypoolrefill(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
	{
        throw std::runtime_error(
            "keypoolrefill ( newsize )\n"
            "\nFills the keypool."
            + HelpRequiringPassphrase() + "\n"
            "\nArguments\n"
            "1. newsize     (numeric, optional, default=1000) The new keypool size\n"
            "\nExamples:\n"
            + HelpExampleCli("keypoolrefill", "")
            + HelpExampleRpc("keypoolrefill", "")
        );
	}
	
    fLiteMode = GetBoolArg("-litemode", false);
    unsigned int nSize;

    if (fLiteMode)
	{
        nSize = std::max(GetArg("-keypool", 1000), (int64_t)0);
	}
    else
	{
        nSize = std::max(GetArg("-keypool", 100), (int64_t)0);
	}
	
    if (params.size() > 0)
	{
        if (params[0].get_int() < 0)
		{
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size");
		}
		
        nSize = (unsigned int) params[0].get_int();
    }

    EnsureWalletIsUnlocked();

    pwalletMain->TopUpKeyPool(nSize);

    if (pwalletMain->GetKeyPoolSize() < nSize)
	{
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");
	}
	
    return json_spirit::Value::null;
}

static void LockWallet(CWallet* pWallet)
{
    LOCK(cs_nWalletUnlockTime);
    
	nWalletUnlockTime = 0;
    pWallet->Lock();
}

json_spirit::Value walletpassphrase(const json_spirit::Array& params, bool fHelp)
{
	if (pwalletMain->IsCrypted() && (fHelp || params.size() < 2 || params.size() > 3))
	{
		throw std::runtime_error(
			"walletpassphrase \"passphrase\" timeout ( anonymizeonly )\n"
			"\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
			"This is needed prior to performing transactions related to private keys such as sending XDN\n"
			"\nArguments:\n"
			"1. \"passphrase\"     (string, required) The wallet passphrase\n"
			"2. timeout            (numeric, required) The time to keep the decryption key in seconds.\n"
			"3. anonymizeonly      (boolean, optional, default=flase) If is true sending functions are disabled."
			"\nNote:\n"
			"Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
			"time that overrides the old one.\n"
			"\nExamples:\n"
			"\nUnlock the wallet for 60 seconds\n"
			+ HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
			"\nLock the wallet again (before 60 seconds)\n"
			+ HelpExampleCli("walletlock", "") +
			"\nAs json rpc call\n"
			+ HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60")
		);
	}

	if (fHelp)
	{
		return true;
	}

	if (!fServer)
	{
		throw JSONRPCError(RPC_SERVER_NOT_STARTED, "Error: RPC server was not started, use server=1 to change this.");
	}

	if (!pwalletMain->IsCrypted())
	{
		throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");
	}

	// Note that the walletpassphrase is stored in params[0] which is not mlock()ed
	SecureString strWalletPass;
	strWalletPass.reserve(100);

	// TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
	// Alternately, find a way to make params[0] mlock()'d to begin with.
	strWalletPass = params[0].get_str().c_str();

	if (strWalletPass.length() > 0)
	{
		if (!pwalletMain->Unlock(strWalletPass))
		{
			throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
		}
	}
	else
	{
		throw std::runtime_error(
			"walletpassphrase <passphrase> <timeout>\n"
			"Stores the wallet decryption key in memory for <timeout> seconds."
		);
	}

	pwalletMain->TopUpKeyPool();

	int64_t nSleepTime = params[1].get_int64();

	// If the timeout value is too large or negative, the conversion from nSleepTime to seconds
	// results in a negative value and the wallet unlocking will fail.
	if (nSleepTime > INT32_MAX || nSleepTime < 0)
	{
		pwalletMain->Lock();
		
		throw JSONRPCError(RPC_INVALID_PARAMETER, "Error: The timeout value entered was incorrect.");
	}

	LOCK(cs_nWalletUnlockTime);

	nWalletUnlockTime = GetTime() + nSleepTime;

	RPCRunLater("lockwallet", boost::bind(&LockWallet, pwalletMain), nSleepTime);

	// ppcoin: if user OS account compromised prevent trivial sendmoney commands
	if (params.size() > 2)
	{
		fWalletUnlockStakingOnly = params[2].get_bool();
	}
	else
	{
		fWalletUnlockStakingOnly = false;
	}

	return json_spirit::Value::null;
}

json_spirit::Value walletpassphrasechange(const json_spirit::Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 2))
	{
        throw std::runtime_error(
            "walletpassphrasechange \"oldpassphrase\" \"newpassphrase\"\n"
            "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n"
            "\nArguments:\n"
            "1. \"oldpassphrase\"      (string) The current passphrase\n"
            "2. \"newpassphrase\"      (string) The new passphrase\n"
            "\nExamples:\n"
            + HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"")
            + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\"")
        );
	}
	
    if (fHelp)
	{
        return true;
    }
	
	if (!pwalletMain->IsCrypted())
	{
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");
	}
	
    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = params[1].get_str().c_str();

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
	{
        throw std::runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>."
		);
	}
	
    if (!pwalletMain->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass))
	{
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
	}
	
    return json_spirit::Value::null;
}

json_spirit::Value walletlock(const json_spirit::Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 0))
	{
        throw std::runtime_error(
            "walletlock\n"
            "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.\n"
            "\nExamples:\n"
            "\nSet the passphrase for 2 minutes to perform a transaction\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n"
            + HelpExampleCli("sendtoaddress", "\"ie6sxvFwLpMsp5tRHpAS6q3cZVewmqYzTg\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("walletlock", "")
        );
	}
	
    if (fHelp)
	{
        return true;
    }
	
	if (!pwalletMain->IsCrypted())
	{
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");
	}
	
    {
        LOCK(cs_nWalletUnlockTime);
        
		pwalletMain->Lock();
        nWalletUnlockTime = 0;
    }

    return json_spirit::Value::null;
}

json_spirit::Value encryptwallet(const json_spirit::Array& params, bool fHelp)
{
    if (!pwalletMain->IsCrypted() && (fHelp || params.size() != 1))
	{
        throw std::runtime_error(
            "encryptwallet \"passphrase\"\n"
            "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
            "After this, any calls that interact with private keys such as sending or signing \n"
            "will require the passphrase to be set prior the making these calls.\n"
            "Use the walletpassphrase call for this, and then walletlock call.\n"
            "If the wallet is already encrypted, use the walletpassphrasechange call.\n"
            "Note that this will shutdown the server.\n"
            "\nArguments:\n"
            "1. \"passphrase\"    (string) The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long.\n"
            "\nExamples:\n"
            "\nEncrypt you wallet\n"
            + HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the wallet, such as for signing or sending DigitalNote\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
            "\nNow we can so something like sign\n"
            + HelpExampleCli("signmessage", "\"DigitalNoteaddress\" \"test message\"") +
            "\nNow lock the wallet again by removing the passphrase\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("encryptwallet", "\"my pass phrase\"")
        );
	}
	
    if (fHelp)
	{
        return true;
    }
	
	if (pwalletMain->IsCrypted())
	{
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");
	}
	
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = params[0].get_str().c_str();

    if (strWalletPass.length() < 1)
	{
        throw std::runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>."
		);
	}
	
    if (!pwalletMain->EncryptWallet(strWalletPass))
	{
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");
	}
	
    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys. So:
    StartShutdown();
    
	return "wallet encrypted; DigitalNote server stopping, restart to run with encrypted wallet. The keypool has been flushed, you need to make a new backup.";
}

// ppcoin: reserve balance from being staked for network protection
json_spirit::Value reservebalance(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
	{
        throw std::runtime_error(
            "reservebalance [<reserve> [amount]]\n"
            "<reserve> is true or false to turn balance reserve on or off.\n"
            "<amount> is a real and rounded to cent.\n"
            "Set reserve amount not participating in network protection.\n"
            "If no parameters provided current setting is printed.\n"
		);
	}
	
    if (params.size() > 0)
    {
        bool fReserve = params[0].get_bool();
        if (fReserve)
        {
            if (params.size() == 1)
			{
                throw std::runtime_error("must provide amount to reserve balance.\n");
            }
			
			CAmount nAmount = AmountFromValue(params[1]);
            nAmount = (nAmount / CENT) * CENT;  // round to cent
            
			if (nAmount < 0)
			{
                throw std::runtime_error("amount cannot be negative.\n");
            }
			
			nReserveBalance = nAmount;
        }
        else
        {
            if (params.size() > 1)
			{
                throw std::runtime_error("cannot specify amount to turn off reserve.\n");
            }
			
			nReserveBalance = 0;
        }
    }

    json_spirit::Object result;
    result.push_back(json_spirit::Pair("reserve", (nReserveBalance > 0)));
    result.push_back(json_spirit::Pair("amount", ValueFromAmount(nReserveBalance)));
    
	return result;
}

// ppcoin: check wallet integrity
json_spirit::Value checkwallet(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
	{
        throw std::runtime_error(
            "checkwallet\n"
            "Check wallet for integrity.\n"
		);
	}
	
    int nMismatchSpent;
    int64_t nBalanceInQuestion;
    pwalletMain->FixSpentCoins(nMismatchSpent, nBalanceInQuestion, true);
    json_spirit::Object result;
    
	if (nMismatchSpent == 0)
	{
        result.push_back(json_spirit::Pair("wallet check passed", true));
	}
    else
    {
        result.push_back(json_spirit::Pair("mismatched spent coins", nMismatchSpent));
        result.push_back(json_spirit::Pair("amount in question", ValueFromAmount(nBalanceInQuestion)));
    }
    
	return result;
}

// ppcoin: repair wallet
json_spirit::Value repairwallet(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
	{
        throw std::runtime_error(
            "repairwallet\n"
            "Repair wallet if checkwallet reports any problem.\n"
		);
	}
	
    int nMismatchSpent;
    int64_t nBalanceInQuestion;
    pwalletMain->FixSpentCoins(nMismatchSpent, nBalanceInQuestion);
    json_spirit::Object result;
    
	if (nMismatchSpent == 0)
	{
        result.push_back(json_spirit::Pair("wallet check passed", true));
	}
    else
    {
        result.push_back(json_spirit::Pair("mismatched spent coins", nMismatchSpent));
        result.push_back(json_spirit::Pair("amount affected by repair", ValueFromAmount(nBalanceInQuestion)));
    }
	
    return result;
}

// NovaCoin: resend unconfirmed wallet transactions
json_spirit::Value resendtx(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
	{
        throw std::runtime_error(
            "resendtx\n"
            "Re-send unconfirmed transactions.\n"
        );
	}
	
    ResendWalletTransactions(true);

    return json_spirit::Value::null;
}

// ppcoin: make a public-private key pair
json_spirit::Value makekeypair(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
	{
        throw std::runtime_error(
            "makekeypair [prefix]\n"
            "Make a public/private key pair.\n"
            "[prefix] is optional preferred prefix for the public key.\n"
		);
	}
	
    std::string strPrefix = "";
    if (params.size() > 0)
	{
        strPrefix = params[0].get_str();
	}
	
    CKey key;
    key.MakeNewKey(false);

    CPrivKey vchPrivKey = key.GetPrivKey();
    
	json_spirit::Object result;
    result.push_back(json_spirit::Pair("PrivateKey", HexStr<CPrivKey::iterator>(vchPrivKey.begin(), vchPrivKey.end())));
    result.push_back(json_spirit::Pair("PublicKey", HexStr(key.GetPubKey())));
	
    return result;
}

json_spirit::Value settxfee(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
	{
        throw std::runtime_error(
            "settxfee amount\n"
            "\nSet the transaction fee per kB.\n"
            "\nArguments:\n"
            "1. amount         (numeric, required) The transaction fee in XDN/kB rounded to the nearest 0.00000001\n"
            "\nResult\n"
            "true|false        (boolean) Returns true if successful\n"
            "\nExamples:\n"
            + HelpExampleCli("settxfee", "0.00001")
            + HelpExampleRpc("settxfee", "0.00001")
        );
	}
	
    nTransactionFee = AmountFromValue(params[0]);
    nTransactionFee = (nTransactionFee / CENT) * CENT;  // round to cent

    return true;
}

json_spirit::Value getnewstealthaddress(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
	{
        throw std::runtime_error(
            "getnewstealthaddress [label]\n"
            "Returns a new DigitalNote stealth address for receiving payments anonymously."
		);
	}
	
    if (pwalletMain->IsLocked())
	{
        throw std::runtime_error("Failed: Wallet must be unlocked.");
	}
	
    std::string sLabel;
    if (params.size() > 0)
	{
        sLabel = params[0].get_str();
	}
	
    CStealthAddress sxAddr;
    std::string sError;
    
	if (!pwalletMain->NewStealthAddress(sError, sLabel, sxAddr))
	{
        throw std::runtime_error(std::string("Could get new stealth address: ") + sError);
	}
	
    if (!pwalletMain->AddStealthAddress(sxAddr))
	{
        throw std::runtime_error("Could not save to wallet.");
	}
	
    return sxAddr.Encoded();
}

json_spirit::Value liststealthaddresses(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
	{
        throw std::runtime_error(
            "liststealthaddresses [show_secrets=0]\n"
            "List owned stealth addresses."
		);
	}
	
    bool fShowSecrets = false;

    if (params.size() > 0)
    {
        std::string str = params[0].get_str();

        if (str == "0" || str == "n" || str == "no" || str == "-" || str == "false")
		{
            fShowSecrets = false;
		}
        else
		{
            fShowSecrets = true;
		}
    }

    if (fShowSecrets)
    {
        if (pwalletMain->IsLocked())
		{
            throw std::runtime_error("Failed: Wallet must be unlocked.");
		}
    }

    json_spirit::Object result;

    //std::set<CStealthAddress>::iterator it;
    //for (it = pwalletMain->stealthAddresses.begin(); it != pwalletMain->stealthAddresses.end(); ++it)
    for(CStealthAddress sit : pwalletMain->stealthAddresses)
    {
		CStealthAddress* it = &(sit);
        
		if (it->scan_secret.size() < 1)
		{
            continue; // stealth address is not owned
		}
		
        if (fShowSecrets)
        {
            json_spirit::Object objA;
            
			objA.push_back(json_spirit::Pair("Label        ", it->label));
            objA.push_back(json_spirit::Pair("Address      ", it->Encoded()));
            objA.push_back(json_spirit::Pair("Scan Secret  ", HexStr(it->scan_secret.begin(), it->scan_secret.end())));
            objA.push_back(json_spirit::Pair("Spend Secret ", HexStr(it->spend_secret.begin(), it->spend_secret.end())));
            
			result.push_back(json_spirit::Pair("Stealth Address", objA));
        }
		else
        {
            result.push_back(json_spirit::Pair("Stealth Address", it->Encoded() + " - " + it->label));
        }
    }

    return result;
}

json_spirit::Value importstealthaddress(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2)
	{
        throw std::runtime_error(
            "importstealthaddress <scan_secret> <spend_secret> [label]\n"
            "Import an owned stealth addresses."
		);
	}
	
    std::string sScanSecret  = params[0].get_str();
    std::string sSpendSecret = params[1].get_str();
    std::string sLabel;
	
    if (params.size() > 2)
    {
        sLabel = params[2].get_str();
    }

    std::vector<uint8_t> vchScanSecret;
    std::vector<uint8_t> vchSpendSecret;

    if (IsHex(sScanSecret))
    {
        vchScanSecret = ParseHex(sScanSecret);
    }
	else
    {
        if (!DecodeBase58(sScanSecret, vchScanSecret))
		{
            throw std::runtime_error("Could not decode scan secret as hex or base58.");
		}
    }

    if (IsHex(sSpendSecret))
    {
        vchSpendSecret = ParseHex(sSpendSecret);
    }
	else
    {
        if (!DecodeBase58(sSpendSecret, vchSpendSecret))
		{
            throw std::runtime_error("Could not decode spend secret as hex or base58.");
		}
    }

    if (vchScanSecret.size() != 32)
	{
        throw std::runtime_error("Scan secret is not 32 bytes.");
    }
	
	if (vchSpendSecret.size() != 32)
	{
        throw std::runtime_error("Spend secret is not 32 bytes.");
	}
	
    ec_secret scan_secret;
    ec_secret spend_secret;

    memcpy(&scan_secret.e[0], &vchScanSecret[0], 32);
    memcpy(&spend_secret.e[0], &vchSpendSecret[0], 32);

    ec_point scan_pubkey, spend_pubkey;
    if (SecretToPublicKey(scan_secret, scan_pubkey) != 0)
	{
        throw std::runtime_error("Could not get scan public key.");
	}
	
    if (SecretToPublicKey(spend_secret, spend_pubkey) != 0)
	{
        throw std::runtime_error("Could not get spend public key.");
	}
	
    CStealthAddress sxAddr;
	
    sxAddr.label = sLabel;
    sxAddr.scan_pubkey = scan_pubkey;
    sxAddr.spend_pubkey = spend_pubkey;
    sxAddr.scan_secret = vchScanSecret;
    sxAddr.spend_secret = vchSpendSecret;

    json_spirit::Object result;
    bool fFound = false;
    
	// -- find if address already exists
    //std::set<CStealthAddress>::iterator it;
    //for (it = pwalletMain->stealthAddresses.begin(); it != pwalletMain->stealthAddresses.end(); ++it)
    for(CStealthAddress it : pwalletMain->stealthAddresses)
    {
        CStealthAddress &sxAddrIt = const_cast<CStealthAddress&>(it); //*it);
        
		if (sxAddrIt.scan_pubkey == sxAddr.scan_pubkey && sxAddrIt.spend_pubkey == sxAddr.spend_pubkey)
        {
            if (sxAddrIt.scan_secret.size() < 1)
            {
                sxAddrIt.scan_secret = sxAddr.scan_secret;
                sxAddrIt.spend_secret = sxAddr.spend_secret;
                
				fFound = true; // update stealth address with secrets
                
				break;
            }

            result.push_back(json_spirit::Pair("result", "Import failed - stealth address exists."));
            
			return result;
        }
    }

    if (fFound)
    {
        result.push_back(json_spirit::Pair("result", "Success, updated " + sxAddr.Encoded()));
    }
	else
    {
        pwalletMain->stealthAddresses.insert(sxAddr);
        
		result.push_back(json_spirit::Pair("result", "Success, imported " + sxAddr.Encoded()));
    }

    if (!pwalletMain->AddStealthAddress(sxAddr))
	{
        throw std::runtime_error("Could not save to wallet.");
	}
	
    return result;
}

json_spirit::Value sendtostealthaddress(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 5)
	{
        throw std::runtime_error(
            "sendtostealthaddress <stealth_address> <amount> [comment] [comment-to] [narration]\n"
            "<amount> is a real and is rounded to the nearest 0.000001"
            + HelpRequiringPassphrase()
		);
	}
	
    if (pwalletMain->IsLocked())
	{
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
	}
	
    std::string sEncoded = params[0].get_str();
    CAmount nAmount = AmountFromValue(params[1]);

    std::string sNarr;
    if (params.size() > 4 && params[4].type() != json_spirit::null_type && !params[4].get_str().empty())
	{
        sNarr = params[4].get_str();
	}
	
    if (sNarr.length() > 24)
	{
        throw std::runtime_error("Narration must be 24 characters or less.");
	}
	
    CStealthAddress sxAddr;
    json_spirit::Object result;

    if (!sxAddr.SetEncoded(sEncoded))
    {
        result.push_back(json_spirit::Pair("result", "Invalid DigitalNote stealth address."));
        
		return result;
    }
	
    CWalletTx wtx;
    if (params.size() > 2 && params[2].type() != json_spirit::null_type && !params[2].get_str().empty())
	{
        wtx.mapValue["comment"] = params[2].get_str();
    }
	
	if (params.size() > 3 && params[3].type() != json_spirit::null_type && !params[3].get_str().empty())
	{
        wtx.mapValue["to"]      = params[3].get_str();
	}
	
    std::string sError;
    if (!pwalletMain->SendStealthMoneyToDestination(sxAddr, nAmount, sNarr, wtx, sError))
	{
        throw JSONRPCError(RPC_WALLET_ERROR, sError);
	}
	
    return wtx.GetHash().GetHex();

    result.push_back(json_spirit::Pair("result", "Not implemented yet."));

    return result;
}

json_spirit::Value scanforalltxns(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
	{
        throw std::runtime_error(
            "scanforalltxns [fromHeight]\n"
            "Scan blockchain for owned transactions."
		);
	}
	
    json_spirit::Object result;
    int32_t nFromHeight = 0;

    CBlockIndex *pindex = pindexGenesisBlock;
	
    if (params.size() > 0)
	{
        nFromHeight = params[0].get_int();
	}
	
    if (nFromHeight > 0)
    {
        pindex = mapBlockIndex[hashBestChain];
        
		while (pindex->nHeight > nFromHeight && pindex->pprev)
		{
            pindex = pindex->pprev;
		}
    }

    if (pindex == NULL)
	{
        throw std::runtime_error("Genesis Block is not set.");
	}
	
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);

        pwalletMain->MarkDirty();

        pwalletMain->ScanForWalletTransactions(pindex, true);
        pwalletMain->ReacceptWalletTransactions();
    }

    result.push_back(json_spirit::Pair("result", "Scan complete."));

    return result;
}

json_spirit::Value scanforstealthtxns(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
	{
        throw std::runtime_error(
            "scanforstealthtxns [fromHeight]\n"
            "Scan blockchain for owned stealth transactions."
		);
	}
	
    json_spirit::Object result;
    uint32_t nBlocks = 0;
    uint32_t nTransactions = 0;
    int32_t nFromHeight = 0;

    CBlockIndex *pindex = pindexGenesisBlock;

	if (params.size() > 0)
	{
        nFromHeight = params[0].get_int();
	}
	
    if (nFromHeight > 0)
    {
        pindex = mapBlockIndex[hashBestChain];
        
		while (pindex->nHeight > nFromHeight && pindex->pprev)
		{
            pindex = pindex->pprev;
		}
    }

    if (pindex == NULL)
	{
        throw std::runtime_error("Genesis Block is not set.");
	}
	
    // -- locks in AddToWalletIfInvolvingMe

    bool fUpdate = true; // todo: option?

    pwalletMain->nStealth = 0;
    pwalletMain->nFoundStealth = 0;

    while (pindex)
    {
        nBlocks++;
        CBlock block;
        block.ReadFromDisk(pindex, true);

        for(CTransaction& tx : block.vtx)
        {
            nTransactions++;

            pwalletMain->AddToWalletIfInvolvingMe(tx, &block, fUpdate);
        }

        pindex = pindex->pnext;
    }

    printf("Scanned %u blocks, %u transactions\n", nBlocks, nTransactions);
    printf("Found %u stealth transactions in blockchain.\n", pwalletMain->nStealth);
    printf("Found %u new owned stealth transactions.\n", pwalletMain->nFoundStealth);

    char cbuf[256];
    snprintf(cbuf, sizeof(cbuf), "%u new stealth transactions.", pwalletMain->nFoundStealth);

    result.push_back(json_spirit::Pair("result", "Scan complete."));
    result.push_back(json_spirit::Pair("found", std::string(cbuf)));

    return result;
}

json_spirit::Value cclistcoins(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
	{
        throw std::runtime_error(
            "cclistcoins\n"
			"CoinControl: list your spendable coins and their information\n"
		);
	}
	
	json_spirit::Array result;

	std::vector<COutput> vCoins;
    pwalletMain->AvailableCoins(vCoins);

    for(const COutput& out : vCoins)
    {
		json_spirit::Object coutput;
		int64_t nHeight = nBestHeight - out.nDepth;
		CBlockIndex* pindex = FindBlockByHeight(nHeight);

		CTxDestination outputAddress;
		ExtractDestination(out.tx->vout[out.i].scriptPubKey, outputAddress);
		
		coutput.push_back(json_spirit::Pair("Address", CDigitalNoteAddress(outputAddress).ToString()));
		coutput.push_back(json_spirit::Pair("Output Hash", out.tx->GetHash().ToString()));
		coutput.push_back(json_spirit::Pair("blockIndex", out.i));
		
		double dAmount = double(out.tx->vout[out.i].nValue) / double(COIN);
		
		coutput.push_back(json_spirit::Pair("Value", dAmount));
		coutput.push_back(json_spirit::Pair("Confirmations", int(out.nDepth)));
		
		double dAge = double(GetTime() - pindex->nTime);
		coutput.push_back(json_spirit::Pair("Age (days)", (dAge/(60*60*24))));
		
		uint64_t nWeight = 0;
		pwalletMain->GetStakeWeightFromValue(out.tx->GetTxTime(), out.tx->vout[out.i].nValue, nWeight);
		
		if(dAge < nStakeMinAge)
		{
				nWeight = 0;
		}
		
		double nReward = (double)GetProofOfStakeReward(pindexBest->pprev, 0, 0);
		
		coutput.push_back(json_spirit::Pair("Weight", int(nWeight)));
		coutput.push_back(json_spirit::Pair("Potential Stake", nReward));
		
		result.push_back(coutput);
	}
	
	return result;
}
