SOURCES += src/bitcoind.cpp

## Fix order to get table correctly
!win32 {
	SOURCES += src/crpctable.cpp
}

SOURCES += src/caddrman.cpp
SOURCES += src/caddrinfo.cpp
SOURCES += src/cinv.cpp
SOURCES += src/caddress.cpp
SOURCES += src/cmessageheader.cpp
SOURCES += src/cregtestparams.cpp
SOURCES += src/ctestnetparams.cpp
SOURCES += src/cmainparams.cpp
SOURCES += src/cchainparams.cpp
SOURCES += src/eccverifyhandle.cpp
SOURCES += src/cextpubkey.cpp
SOURCES += src/secp256k1_context_verify.cpp
SOURCES += src/cpubkey.cpp
SOURCES += src/ckey.cpp
SOURCES += src/cextkey.cpp
SOURCES += src/ccryptokeystore.cpp
SOURCES += src/ccrypter.cpp
SOURCES += src/cmasterkey.cpp
SOURCES += src/ckeystore.cpp
SOURCES += src/cbasickeystore.cpp
SOURCES += src/chash256.cpp
SOURCES += src/chash160.cpp
SOURCES += src/chashwriter.cpp
SOURCES += src/cdb.cpp
SOURCES += src/cdbenv.cpp
SOURCES += src/cinpoint.cpp
SOURCES += src/coutpoint.cpp
SOURCES += src/ctxin.cpp
SOURCES += src/ctxout.cpp
SOURCES += src/csporkmanager.cpp
SOURCES += src/csporkmessage.cpp
SOURCES += src/cconsensusvote.cpp
SOURCES += src/ctransactionlock.cpp
SOURCES += src/cunsignedalert.cpp
SOURCES += src/cstealthkeymetadata.cpp
SOURCES += src/ckeymetadata.cpp
SOURCES += src/cstealthaddress.cpp
SOURCES += src/cscriptcompressor.cpp
SOURCES += src/cscriptvisitor.cpp
SOURCES += src/cscript.cpp
SOURCES += src/cscriptnum.cpp
SOURCES += src/caffectedkeysvisitor.cpp
SOURCES += src/ckeystoreisminevisitor.cpp
SOURCES += src/csignaturecache.cpp
SOURCES += src/signaturechecker.cpp
SOURCES += src/caccountingentry.cpp
SOURCES += src/caccount.cpp
SOURCES += src/cwalletkey.cpp
SOURCES += src/coutput.cpp
SOURCES += src/cwallettx.cpp
SOURCES += src/creservekey.cpp
SOURCES += src/cwallet.cpp
SOURCES += src/ckeypool.cpp
SOURCES += src/cvalidationstate.cpp
SOURCES += src/cblocklocator.cpp
SOURCES += src/cdiskblockindex.cpp
SOURCES += src/cblockindex.cpp
SOURCES += src/cdiskblockpos.cpp
SOURCES += src/cblock.cpp
SOURCES += src/ctxoutcompressor.cpp
SOURCES += src/ctxindex.cpp
SOURCES += src/cmerkletx.cpp
SOURCES += src/cdisktxpos.cpp
SOURCES += src/ctransaction.cpp
SOURCES += src/calert.cpp
SOURCES += src/blocksizecalculator.cpp
SOURCES += src/blockparams.cpp
SOURCES += src/chainparams.cpp
SOURCES += src/version.cpp
SOURCES += src/velocity.cpp
SOURCES += src/ctxmempool.cpp
SOURCES += src/util.cpp
SOURCES += src/hash.cpp
SOURCES += src/netbase.cpp
SOURCES += src/ecwrapper.cpp
SOURCES += src/pubkey.cpp
SOURCES += src/script.cpp
SOURCES += src/scrypt.cpp
SOURCES += src/main.cpp
SOURCES += src/miner.cpp
SOURCES += src/init.cpp
SOURCES += src/net.cpp
SOURCES += src/checkpoints.cpp
SOURCES += src/db.cpp
SOURCES += src/walletdb.cpp
SOURCES += src/txdb-leveldb.cpp
SOURCES += src/wallet.cpp
SOURCES += src/rpcclient.cpp
SOURCES += src/ssliostreamdevice.cpp
SOURCES += src/rpcprotocol.cpp
SOURCES += src/rpcserver.cpp
SOURCES += src/rpcdump.cpp
SOURCES += src/rpcmisc.cpp
SOURCES += src/describeaddressvisitor.cpp
SOURCES += src/rpcnet.cpp
SOURCES += src/rpcmining.cpp
SOURCES += src/rpcvelocity.cpp
SOURCES += src/rpcwallet.cpp
SOURCES += src/rpcblockchain.cpp
SOURCES += src/rpcrawtransaction.cpp
SOURCES += src/crypter.cpp
SOURCES += src/noui.cpp
SOURCES += src/kernel.cpp
SOURCES += src/pbkdf2.cpp
SOURCES += src/stealth.cpp
SOURCES += src/instantx.cpp
SOURCES += src/spork.cpp
SOURCES += src/smsg.cpp
SOURCES += src/webwalletconnector.cpp
SOURCES += src/rpcsmessage.cpp
SOURCES += src/ccoincontrol.cpp
SOURCES += src/ui_translate.cpp
SOURCES += src/limitedmap.cpp
SOURCES += src/mruset.cpp
SOURCES += src/cautofile.cpp
SOURCES += src/csizecomputer.cpp
SOURCES += src/cdatastream.cpp
SOURCES += src/cflatdata.cpp
SOURCES += src/cvarint.cpp
SOURCES += src/fork.cpp

SOURCES += src/cbignum_ctx.cpp
SOURCES += src/cbignum.cpp

SOURCES += src/cbase58data.cpp
SOURCES += src/cdigitalnoteaddress.cpp
SOURCES += src/cdigitalnoteaddressvisitor.cpp
SOURCES += src/cdigitalnotesecret.cpp
SOURCES += src/cbitcoinaddress.cpp
SOURCES += src/cbitcoinaddressvisitor.cpp
SOURCES += src/base58.cpp

SOURCES += src/ctxdsin.cpp
SOURCES += src/ctxdsout.cpp
SOURCES += src/cmnengineentry.cpp
SOURCES += src/cmnenginequeue.cpp
SOURCES += src/cmnenginesigner.cpp
SOURCES += src/cmnenginepool.cpp
SOURCES += src/mnengine.cpp
SOURCES += src/rpcmnengine.cpp

SOURCES += src/cmasternode.cpp
SOURCES += src/cmasternodeman.cpp
SOURCES += src/cmasternodedb.cpp
SOURCES += src/cmasternodepaymentwinner.cpp
SOURCES += src/cmasternodepayments.cpp
SOURCES += src/cmasternodeconfig.cpp
SOURCES += src/cmasternodeconfigentry.cpp
SOURCES += src/cactivemasternode.cpp
SOURCES += src/masternode.cpp
SOURCES += src/masternodeman.cpp
SOURCES += src/masternodeconfig.cpp
SOURCES += src/masternode-payments.cpp

SOURCES += src/allocators.cpp
SOURCES += src/allocators/lockedpagemanagerbase.cpp
SOURCES += src/allocators/lockedpagemanager.cpp
SOURCES += src/allocators/memorypagelocker.cpp
SOURCES += src/allocators/secure_allocator.cpp
SOURCES += src/allocators/zero_after_free_allocator.cpp

SOURCES += src/thread.cpp
SOURCES += src/thread/cmutexlock.cpp
SOURCES += src/thread/csemaphore.cpp
SOURCES += src/thread/csemaphoregrant.cpp

SOURCES += src/smsg/address.cpp
SOURCES += src/smsg/batchscanner.cpp
SOURCES += src/smsg/bucket.cpp
SOURCES += src/smsg/ckeyid_b.cpp
SOURCES += src/smsg/cdigitalnoteaddress_b.cpp
SOURCES += src/smsg/crypter.cpp
SOURCES += src/smsg/db.cpp
SOURCES += src/smsg/options.cpp
SOURCES += src/smsg/token.cpp
SOURCES += src/smsg/stored.cpp
SOURCES += src/smsg/securemessage.cpp

SOURCES += src/net/cservice.cpp
SOURCES += src/net/csubnet.cpp
SOURCES += src/net/cnetaddr.cpp
SOURCES += src/net/caddrdb.cpp
SOURCES += src/net/cbandb.cpp
SOURCES += src/net/cbanentry.cpp
SOURCES += src/net/cnetmessage.cpp
SOURCES += src/net/cnode.cpp

SOURCES += src/uint/uint_base.cpp
SOURCES += src/uint/uint160.cpp
SOURCES += src/uint/uint256.cpp
SOURCES += src/uint/uint512.cpp

SOURCES += src/support/cleanse.cpp

SOURCES += src/serialize/base.cpp
SOURCES += src/serialize/read.cpp
SOURCES += src/serialize/write.cpp
SOURCES += src/serialize/string.cpp
SOURCES += src/serialize/vector.cpp
SOURCES += src/serialize/pair.cpp
SOURCES += src/serialize/tuple.cpp
SOURCES += src/serialize/map.cpp
SOURCES += src/serialize/set.cpp

SOURCES += src/crypto/common/hmac_sha256.cpp
SOURCES += src/crypto/common/hmac_sha512.cpp
SOURCES += src/crypto/common/ripemd160.cpp
SOURCES += src/crypto/common/sha1.cpp
SOURCES += src/crypto/common/sha256.cpp
SOURCES += src/crypto/common/sha512.cpp
SOURCES += src/crypto/common/aes_helper.c
SOURCES += src/crypto/common/bmw.c
SOURCES += src/crypto/common/echo.c

SOURCES += src/rpcmintblock.cpp
SOURCES += src/rpcdebug.cpp

## Fix order to get table correctly
win32 {
	SOURCES += src/crpctable.cpp
}