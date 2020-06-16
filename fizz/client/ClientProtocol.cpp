/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <fizz/client/ClientProtocol.h>

#include <fizz/client/PskCache.h>
#include <fizz/client/State.h>
#include <fizz/crypto/Utils.h>
#include <fizz/protocol/CertificateVerifier.h>
#include <fizz/protocol/Protocol.h>
#include <fizz/protocol/StateMachine.h>
#include <fizz/record/Extensions.h>

using folly::Optional;

using namespace fizz::client;
using namespace fizz::client::detail;

namespace fizz {
namespace sm {

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::Uninitialized,
    Event::Connect,
    StateEnum::ExpectingServerHello);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingServerHello,
    Event::HelloRetryRequest,
    StateEnum::ExpectingServerHello);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingServerHello,
    Event::ServerHello,
    StateEnum::ExpectingEncryptedExtensions);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingServerHello,
    Event::EarlyAppWrite,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingEncryptedExtensions,
    Event::EncryptedExtensions,
    StateEnum::ExpectingCertificate,
    StateEnum::ExpectingFinished);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingEncryptedExtensions,
    Event::EarlyAppWrite,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingCertificate,
    Event::CertificateRequest,
    StateEnum::ExpectingCertificate);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingCertificate,
    Event::Certificate,
    StateEnum::ExpectingCertificateVerify);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingCertificate,
    Event::CompressedCertificate,
    StateEnum::ExpectingCertificateVerify);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingCertificate,
    Event::EarlyAppWrite,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingCertificateVerify,
    Event::CertificateVerify,
    StateEnum::ExpectingFinished);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingCertificateVerify,
    Event::EarlyAppWrite,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingFinished,
    Event::Finished,
    StateEnum::Established);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingFinished,
    Event::EarlyAppWrite,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::Established,
    Event::EarlyAppWrite,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::Established,
    Event::NewSessionTicket,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::Established,
    Event::AppData,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::Established,
    Event::AppWrite,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::Established,
    Event::KeyUpdate,
    StateEnum::Error);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::Established,
    Event::CloseNotify,
    StateEnum::Closed);

FIZZ_DECLARE_EVENT_HANDLER(
    ClientTypes,
    StateEnum::ExpectingCloseNotify,
    Event::CloseNotify,
    StateEnum::Closed);
} // namespace sm

namespace client {

Actions ClientStateMachine::processConnect(
    const State& state,
    std::shared_ptr<const FizzClientContext> context,
    std::shared_ptr<const CertificateVerifier> verifier,
    Optional<std::string> sni,
    Optional<CachedPsk> cachedPsk,
    const std::shared_ptr<ClientExtensions>& extensions) {
  Connect connect;
  connect.context = std::move(context);
  connect.sni = std::move(sni);
  connect.verifier = std::move(verifier);
  connect.extensions = extensions;
  connect.cachedPsk = std::move(cachedPsk);
  return detail::processEvent(state, std::move(connect));
}

Actions ClientStateMachine::processSocketData(
    const State& state,
    folly::IOBufQueue& buf) {
  try {
    if (!state.readRecordLayer()) {
      return detail::handleError(
          state,
          ReportError("attempting to process data without record layer"),
          folly::none);
    }
    auto param = state.readRecordLayer()->readEvent(buf);
    if (!param.has_value()) {
      return actions(WaitForData());
    }
    return detail::processEvent(state, std::move(*param));
  } catch (const FizzException& e) {
    return detail::handleError(
        state,
        ReportError(folly::exception_wrapper(std::current_exception(), e)),
        e.getAlert());
  } catch (const std::exception& e) {
    return detail::handleError(
        state,
        ReportError(folly::make_exception_wrapper<FizzException>(
            folly::to<std::string>(
                "error decoding record in state ",
                toString(state.state()),
                ": ",
                e.what()),
            AlertDescription::decode_error)),
        AlertDescription::decode_error);
  }
}

Actions ClientStateMachine::processWriteNewSessionTicket(
    const State& state,
    WriteNewSessionTicket write) {
  return detail::processEvent(state, std::move(write));
}

Actions ClientStateMachine::processAppWrite(
    const State& state,
    AppWrite write) {
  return detail::processEvent(state, std::move(write));
}

Actions ClientStateMachine::processEarlyAppWrite(
    const State& state,
    EarlyAppWrite write) {
  return detail::processEvent(state, std::move(write));
}

Actions ClientStateMachine::processAppClose(const State& state) {
  return detail::handleAppClose(state);
}

Actions ClientStateMachine::processAppCloseImmediate(const State& state) {
  return detail::handleAppCloseImmediate(state);
}

namespace detail {

Actions processEvent(const State& state, Param param) {
  auto event = EventVisitor()(param);
  try {
    return sm::StateMachine<ClientTypes>::getHandler(state.state(), event)(
        state, std::move(param));
  } catch (const FizzException& e) {
    return detail::handleError(
        state,
        ReportError(folly::exception_wrapper(std::current_exception(), e)),
        e.getAlert());
  } catch (const std::exception& e) {
    return detail::handleError(
        state,
        ReportError(folly::exception_wrapper(std::current_exception(), e)),
        AlertDescription::unexpected_message);
  }
}

Actions handleError(
    const State& state,
    ReportError error,
    Optional<AlertDescription> alertDesc) {
  if (state.state() == StateEnum::Error) {
    return actions(std::move(error));
  }
  MutateState transition([](State& newState) {
    newState.state() = StateEnum::Error;
    newState.writeRecordLayer() = nullptr;
    newState.readRecordLayer() = nullptr;
  });
  if (alertDesc && state.writeRecordLayer()) {
    Alert alert(*alertDesc);
    WriteToSocket write;
    write.contents.emplace_back(
        state.writeRecordLayer()->writeAlert(std::move(alert)));
    return actions(std::move(transition), std::move(write), std::move(error));
  } else {
    return actions(std::move(transition), std::move(error));
  }
}

Actions handleAppCloseImmediate(const State& state) {
  MutateState transition([](State& newState) {
    newState.state() = StateEnum::Closed;
    newState.writeRecordLayer() = nullptr;
    newState.readRecordLayer() = nullptr;
  });
  if (state.writeRecordLayer()) {
    Alert alert(AlertDescription::close_notify);
    WriteToSocket write;
    write.contents.emplace_back(
        state.writeRecordLayer()->writeAlert(std::move(alert)));
    return actions(std::move(transition), std::move(write));
  } else {
    return actions(std::move(transition));
  }
}

Actions handleAppClose(const State& state) {
  if (state.writeRecordLayer()) {
    MutateState transition([](State& newState) {
      newState.state() = StateEnum::ExpectingCloseNotify;
      newState.writeRecordLayer() = nullptr;
    });

    Alert alert(AlertDescription::close_notify);
    WriteToSocket write;
    write.contents.emplace_back(
        state.writeRecordLayer()->writeAlert(std::move(alert)));
    return actions(std::move(transition), std::move(write));
  } else {
    MutateState transition([](State& newState) {
      newState.state() = StateEnum::Closed;
      newState.writeRecordLayer() = nullptr;
      newState.readRecordLayer() = nullptr;
    });
    return actions(std::move(transition));
  }
}

Actions handleInvalidEvent(const State& state, Event event, Param param) {
  if (event == Event::Alert) {
    auto& alert = *param.asAlert();
    throw FizzException(
        folly::to<std::string>(
            "received alert: ",
            toString(alert.description),
            ", in state ",
            toString(state.state())),
        folly::none);
  } else {
    throw FizzException(
        folly::to<std::string>(
            "invalid event: ",
            toString(event),
            ", in state ",
            toString(state.state())),
        AlertDescription::unexpected_message);
  }
}

} // namespace detail
} // namespace client

namespace sm {

static void ensureNoUnparsedHandshakeData(const State& state, Event event) {
  if (state.readRecordLayer()->hasUnparsedHandshakeData()) {
    throw FizzException(
        folly::to<std::string>(
            "unprocessed handshake data while handling event ",
            toString(event),
            " in state ",
            toString(state.state())),
        AlertDescription::unexpected_message);
  }
}

static folly::Optional<CachedPsk> validatePsk(
    const FizzClientContext& context,
    folly::Optional<CachedPsk> psk) {
  if (!psk) {
    return folly::none;
  }

  if (std::find(
          context.getSupportedVersions().begin(),
          context.getSupportedVersions().end(),
          psk->version) == context.getSupportedVersions().end()) {
    VLOG(1) << "Ignoring cached psk with protocol version "
            << toString(psk->version);
    return folly::none;
  }
  if (std::find(
          context.getSupportedCiphers().begin(),
          context.getSupportedCiphers().end(),
          psk->cipher) == context.getSupportedCiphers().end()) {
    VLOG(1) << "Ignoring cached psk with cipher " << toString(psk->cipher);
    return folly::none;
  }

  // The below checks apply only to resumption tickets
  if (psk->type == PskType::Resumption) {
    auto now = context.getClock()->getCurrentTime();
    if (now > psk->ticketExpirationTime) {
      VLOG(1) << "Ignoring expired cached psk";
      return folly::none;
    }
    if (now - psk->ticketHandshakeTime > context.getMaxPskHandshakeLife()) {
      VLOG(1) << "Ignoring psk with stale handshake";
      return folly::none;
    }

    if (psk->ticketHandshakeTime > now) {
      VLOG(1) << "Ignoring psk from future";
      return folly::none;
    }
  }

  return psk;
}

static std::map<NamedGroup, std::unique_ptr<KeyExchange>> getKeyExchangers(
    const Factory& factory,
    const std::vector<NamedGroup>& groups) {
  std::map<NamedGroup, std::unique_ptr<KeyExchange>> keyExchangers;
  for (auto group : groups) {
    auto kex = factory.makeKeyExchange(group);
    kex->generateKeyPair();
    keyExchangers.emplace(group, std::move(kex));
  }
  return keyExchangers;
}

static ClientHello getClientHello(
    const Factory& /*factory*/,
    const Random& random,
    const std::vector<CipherSuite>& supportedCiphers,
    const std::vector<ProtocolVersion>& supportedVersions,
    const std::vector<NamedGroup>& supportedGroups,
    const std::map<NamedGroup, std::unique_ptr<KeyExchange>>& shares,
    const std::vector<SignatureScheme>& supportedSigSchemes,
    const std::vector<PskKeyExchangeMode>& supportedPskModes,
    const folly::Optional<std::string>& hostname,
    const std::vector<std::string>& supportedAlpns,
    const std::vector<CertificateCompressionAlgorithm>& compressionAlgos,
    const Optional<EarlyDataParams>& earlyDataParams,
    const Buf& legacySessionId,
    ClientExtensions* extensions,
    Buf cookie = nullptr) {
  ClientHello chlo;
  chlo.legacy_version = ProtocolVersion::tls_1_2;
  chlo.random = random;
  chlo.legacy_session_id = legacySessionId->clone();
  chlo.cipher_suites = supportedCiphers;
  chlo.legacy_compression_methods.push_back(0x00);

  SupportedVersions versions;
  versions.versions = supportedVersions;
  chlo.extensions.push_back(encodeExtension(std::move(versions)));

  SupportedGroups groups;
  groups.named_group_list = supportedGroups;
  chlo.extensions.push_back(encodeExtension(std::move(groups)));

  ClientKeyShare keyShare;
  for (const auto& share : shares) {
    KeyShareEntry entry;
    entry.group = share.first;
    entry.key_exchange = share.second->getKeyShare();
    keyShare.client_shares.push_back(std::move(entry));
  }
  chlo.extensions.push_back(encodeExtension(std::move(keyShare)));

  SignatureAlgorithms sigAlgs;
  sigAlgs.supported_signature_algorithms = supportedSigSchemes;
  chlo.extensions.push_back(encodeExtension(std::move(sigAlgs)));

  if (hostname) {
    ServerNameList sni;
    ServerName sn;
    sn.hostname = folly::IOBuf::copyBuffer(*hostname);
    sni.server_name_list.push_back(std::move(sn));
    chlo.extensions.push_back(encodeExtension(std::move(sni)));
  }

  if (!supportedAlpns.empty()) {
    ProtocolNameList alpn;
    for (const auto& protoName : supportedAlpns) {
      ProtocolName proto;
      proto.name = folly::IOBuf::copyBuffer(protoName);
      alpn.protocol_name_list.push_back(std::move(proto));
    }
    chlo.extensions.push_back(encodeExtension(std::move(alpn)));
  }

  if (!supportedPskModes.empty()) {
    PskKeyExchangeModes modes;
    modes.modes = supportedPskModes;
    chlo.extensions.push_back(encodeExtension(std::move(modes)));
  }

  if (earlyDataParams) {
    chlo.extensions.push_back(encodeExtension(ClientEarlyData()));
  }

  if (cookie) {
    Cookie monster;
    monster.cookie = std::move(cookie);
    chlo.extensions.push_back(encodeExtension(std::move(monster)));
  }

  if (!compressionAlgos.empty()) {
    CertificateCompressionAlgorithms algos;
    algos.algorithms = compressionAlgos;
    chlo.extensions.push_back(encodeExtension(std::move(algos)));
  }

  if (extensions) {
    auto additionalExtensions = extensions->getClientHelloExtensions();
    for (auto& ext : additionalExtensions) {
      chlo.extensions.push_back(std::move(ext));
    }
  }

  return chlo;
}

static ClientPresharedKey getPskExtension(
    const CachedPsk& psk,
    const Clock& clock) {
  ClientPresharedKey pskExt;
  PskIdentity ident;
  ident.psk_identity = folly::IOBuf::copyBuffer(psk.psk);
  if (psk.type == PskType::Resumption) {
    ident.obfuscated_ticket_age =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock.getCurrentTime() - psk.ticketIssueTime)
            .count();
    ident.obfuscated_ticket_age += psk.ticketAgeAdd;
  } else {
    ident.obfuscated_ticket_age = 0;
  }
  pskExt.identities.push_back(std::move(ident));
  PskBinder binder;
  size_t binderSize = getHashSize(getHashFunction(psk.cipher));
  binder.binder = folly::IOBuf::create(binderSize);
  memset(binder.binder->writableData(), 0, binderSize);
  binder.binder->append(binderSize);
  pskExt.binders.push_back(std::move(binder));
  return pskExt;
}

/**
 * Returns the encoded client hello after updating the binder.
 *
 * Will derive the early secret on the key scheduler and append the client hello
 * to the handshake context.
 */
static Buf encodeAndAddBinders(
    ClientHello chlo,
    const CachedPsk& psk,
    KeyScheduler& scheduler,
    HandshakeContext& handshakeContext,
    const Clock& clock) {
  scheduler.deriveEarlySecret(folly::range(psk.secret));

  auto binderKey = scheduler.getSecret(
      psk.type == PskType::External ? EarlySecrets::ExternalPskBinder
                                    : EarlySecrets::ResumptionPskBinder,
      handshakeContext.getBlankContext());

  auto pskExt = getPskExtension(psk, clock);
  chlo.extensions.push_back(encodeExtension(pskExt));

  size_t binderLength = getBinderLength(chlo);

  auto preEncoded = encodeHandshake(chlo);

  // Add the ClientHello up to the binder list to the transcript.
  {
    folly::IOBufQueue chloQueue(folly::IOBufQueue::cacheChainLength());
    chloQueue.append(std::move(preEncoded));
    auto chloPrefix = chloQueue.split(chloQueue.chainLength() - binderLength);
    handshakeContext.appendToTranscript(chloPrefix);
  }

  PskBinder binder;
  binder.binder =
      handshakeContext.getFinishedData(folly::range(binderKey.secret));
  pskExt.binders.clear();
  pskExt.binders.push_back(std::move(binder));

  chlo.extensions.pop_back();
  chlo.extensions.push_back(encodeExtension(std::move(pskExt)));

  auto encoded = encodeHandshake(std::move(chlo));

  // Add the binder list to the transcript.
  folly::IOBufQueue chloQueue(folly::IOBufQueue::cacheChainLength());
  chloQueue.append(encoded->clone());
  chloQueue.split(chloQueue.chainLength() - binderLength);
  handshakeContext.appendToTranscript(chloQueue.move());

  return encoded;
}

static Optional<EarlyDataParams> getEarlyDataParams(
    const FizzClientContext& context,
    const Optional<CachedPsk>& psk) {
  if (!context.getSendEarlyData()) {
    return folly::none;
  }

  if (!psk || psk->maxEarlyDataSize == 0) {
    return folly::none;
  }

  if (psk->alpn &&
      std::find(
          context.getSupportedAlpns().begin(),
          context.getSupportedAlpns().end(),
          *psk->alpn) == context.getSupportedAlpns().end()) {
    return folly::none;
  }

  EarlyDataParams params;
  params.version = psk->version;
  params.cipher = psk->cipher;
  params.serverCert = psk->serverCert;
  params.clientCert = psk->clientCert;
  params.alpn = psk->alpn;
  return params;
}

Actions
EventHandler<ClientTypes, StateEnum::Uninitialized, Event::Connect>::handle(
    const State& /*state*/,
    Param param) {
  auto& connect = *param.asConnect();

  auto context = std::move(connect.context);

  folly::Optional<CachedPsk> psk =
      validatePsk(*context, std::move(connect.cachedPsk));

  auto random = context->getFactory()->makeRandom();

  // If we have a saved PSK, use the group to choose which groups to
  // send by default
  std::vector<NamedGroup> selectedShares;
  if (psk && psk->group &&
      std::find(
          context->getSupportedGroups().begin(),
          context->getSupportedGroups().end(),
          *psk->group) != context->getSupportedGroups().end()) {
    // key exchange done last time
    selectedShares = {*psk->group};
  } else if (psk && !psk->group) {
    // psk_ke last time
    selectedShares = {};
  } else {
    selectedShares = context->getDefaultShares();
  }

  auto earlyDataParams = getEarlyDataParams(*context, psk);

  Buf legacySessionId;
  if (context->getCompatibilityMode()) {
    legacySessionId =
        folly::IOBuf::copyBuffer(context->getFactory()->makeRandom());
  } else {
    legacySessionId = folly::IOBuf::create(0);
  }

  auto keyExchangers = getKeyExchangers(*context->getFactory(), selectedShares);

  auto chlo = getClientHello(
      *context->getFactory(),
      random,
      context->getSupportedCiphers(),
      context->getSupportedVersions(),
      context->getSupportedGroups(),
      keyExchangers,
      context->getSupportedSigSchemes(),
      context->getSupportedPskModes(),
      connect.sni,
      context->getSupportedAlpns(),
      context->getSupportedCertDecompressionAlgorithms(),
      earlyDataParams,
      legacySessionId,
      connect.extensions.get());

  std::vector<ExtensionType> requestedExtensions;
  for (const auto& extension : chlo.extensions) {
    requestedExtensions.push_back(extension.extension_type);
  }

  Buf encodedClientHello;
  std::unique_ptr<EncryptedWriteRecordLayer> earlyWriteRecordLayer;
  Optional<ReportEarlyHandshakeSuccess> reportEarlySuccess;
  Optional<SecretAvailable> earlyWriteSecretAvailable;
  if (psk) {
    requestedExtensions.push_back(ExtensionType::pre_shared_key);
    auto keyScheduler = context->getFactory()->makeKeyScheduler(psk->cipher);
    auto handshakeContext =
        context->getFactory()->makeHandshakeContext(psk->cipher);

    encodedClientHello = encodeAndAddBinders(
        std::move(chlo),
        *psk,
        *keyScheduler,
        *handshakeContext,
        *context->getClock());

    if (earlyDataParams) {
      auto earlyWriteSecret = keyScheduler->getSecret(
          EarlySecrets::ClientEarlyTraffic,
          handshakeContext->getHandshakeContext()->coalesce());
      if (!context->getOmitEarlyRecordLayer()) {
        earlyWriteRecordLayer =
            context->getFactory()->makeEncryptedWriteRecordLayer(
                EncryptionLevel::EarlyData);
        earlyWriteRecordLayer->setProtocolVersion(psk->version);
        Protocol::setAead(
            *earlyWriteRecordLayer,
            psk->cipher,
            folly::range(earlyWriteSecret.secret),
            *context->getFactory(),
            *keyScheduler);
      }
      earlyWriteSecretAvailable = SecretAvailable(std::move(earlyWriteSecret));

      auto earlyExporterVector = keyScheduler->getSecret(
          EarlySecrets::EarlyExporter,
          handshakeContext->getHandshakeContext()->coalesce());
      earlyDataParams->earlyExporterSecret =
          folly::IOBuf::copyBuffer(folly::range(earlyExporterVector.secret));

      reportEarlySuccess = ReportEarlyHandshakeSuccess();
      reportEarlySuccess->maxEarlyDataSize = psk->maxEarlyDataSize;
    }
  } else {
    encodedClientHello = encodeHandshake(std::move(chlo));
  }

  auto readRecordLayer = context->getFactory()->makePlaintextReadRecordLayer();
  auto writeRecordLayer =
      context->getFactory()->makePlaintextWriteRecordLayer();

  WriteToSocket write;
  write.contents.emplace_back(
      writeRecordLayer->writeInitialClientHello(encodedClientHello->clone()));

  EarlyDataType earlyDataType =
      earlyDataParams ? EarlyDataType::Attempted : EarlyDataType::NotAttempted;

  MutateState saveState([context = std::move(context),
                         verifier = connect.verifier,
                         encodedClientHello = std::move(encodedClientHello),
                         readRecordLayer = std::move(readRecordLayer),
                         writeRecordLayer = std::move(writeRecordLayer),
                         keyExchangers = std::move(keyExchangers),
                         sni = std::move(connect.sni),
                         random = std::move(random),
                         legacySessionId = std::move(legacySessionId),
                         psk = std::move(psk),
                         extensions = connect.extensions,
                         requestedExtensions = std::move(requestedExtensions),
                         earlyDataType](State& newState) mutable {
    newState.context() = std::move(context);
    newState.verifier() = verifier;
    newState.encodedClientHello() = std::move(encodedClientHello);
    newState.readRecordLayer() = std::move(readRecordLayer);
    newState.writeRecordLayer() = std::move(writeRecordLayer);
    newState.keyExchangers() = std::move(keyExchangers);
    newState.sni() = std::move(sni);
    newState.clientRandom() = std::move(random);
    newState.legacySessionId() = std::move(legacySessionId);
    newState.attemptedPsk() = std::move(psk);
    newState.extensions() = extensions;
    newState.requestedExtensions() = std::move(requestedExtensions);
    newState.earlyDataType() = earlyDataType;
  });

  if (reportEarlySuccess) {
    return actions(
        std::move(saveState),
        MutateState([earlyDataParams = std::move(*earlyDataParams),
                     earlyWriteRecordLayer = std::move(earlyWriteRecordLayer)](
                        State& newState) mutable {
          newState.earlyDataParams() = std::move(earlyDataParams);
          newState.earlyWriteRecordLayer() = std::move(earlyWriteRecordLayer);
        }),
        std::move(write),
        std::move(*earlyWriteSecretAvailable),
        std::move(*reportEarlySuccess),
        MutateState(&Transition<StateEnum::ExpectingServerHello>));
  } else {
    return actions(
        std::move(saveState),
        std::move(write),
        MutateState(&Transition<StateEnum::ExpectingServerHello>));
  }
}

template <typename ServerMessage>
static std::pair<ProtocolVersion, CipherSuite> getAndValidateVersionAndCipher(
    const ServerMessage& msg,
    const std::vector<ProtocolVersion>& supportedVersions,
    const std::vector<CipherSuite>& supportedCiphers) {
  if (msg.legacy_version != ProtocolVersion::tls_1_2) {
    throw FizzException(
        folly::to<std::string>(
            "received server legacy version ", toString(msg.legacy_version)),
        AlertDescription::protocol_version);
  }

  if (msg.legacy_compression_method != 0x00) {
    throw FizzException(
        "compression method not null", AlertDescription::illegal_parameter);
  }

  auto supportedVersionsExt =
      getExtension<ServerSupportedVersions>(msg.extensions);
  if (!supportedVersionsExt) {
    throw FizzException(
        "no supported versions in shlo", AlertDescription::protocol_version);
  }
  auto selectedVersion = supportedVersionsExt->selected_version;
  auto selectedCipher = msg.cipher_suite;

  if (std::find(
          supportedVersions.begin(),
          supportedVersions.end(),
          selectedVersion) == supportedVersions.end()) {
    throw FizzException(
        "received unsupported server version",
        AlertDescription::protocol_version);
  }
  if (std::find(
          supportedCiphers.begin(), supportedCiphers.end(), selectedCipher) ==
      supportedCiphers.end()) {
    throw FizzException(
        "server choose unsupported cipher suite",
        AlertDescription::handshake_failure);
  }

  return std::make_pair(selectedVersion, selectedCipher);
}

static auto negotiateParameters(
    const ServerHello& shlo,
    const std::vector<ProtocolVersion>& supportedVersions,
    const std::vector<CipherSuite>& supportedCiphers,
    const std::map<NamedGroup, std::unique_ptr<KeyExchange>>& keyExchangers) {
  ProtocolVersion version;
  CipherSuite cipher;
  std::tie(version, cipher) =
      getAndValidateVersionAndCipher(shlo, supportedVersions, supportedCiphers);

  Optional<std::tuple<NamedGroup, Buf, const KeyExchange*>> exchange;
  const auto serverShare = getExtension<ServerKeyShare>(shlo.extensions);
  if (serverShare) {
    auto kex = keyExchangers.find(serverShare->server_share.group);
    if (kex == keyExchangers.end()) {
      throw FizzException(
          "server choose unsupported group",
          AlertDescription::handshake_failure);
    }
    exchange = std::make_tuple(
        serverShare->server_share.group,
        serverShare->server_share.key_exchange->clone(),
        kex->second.get());
  }

  return std::make_tuple(version, cipher, std::move(exchange));
}

static void validateNegotiationConsistency(
    const State& state,
    ProtocolVersion version,
    CipherSuite cipher) {
  if (state.version() && *state.version() != version) {
    throw FizzException(
        "version does not match", AlertDescription::handshake_failure);
  }
  if (state.cipher() && *state.cipher() != cipher) {
    throw FizzException(
        "cipher does not match", AlertDescription::handshake_failure);
  }
}

namespace {
struct NegotiatedPsk {
  PskType type;
  folly::Optional<PskKeyExchangeMode> mode;
  std::shared_ptr<const Cert> serverCert;
  std::shared_ptr<const Cert> clientCert;

  explicit NegotiatedPsk(
      PskType type,
      folly::Optional<PskKeyExchangeMode> mode = folly::none,
      std::shared_ptr<const Cert> serverCert = nullptr,
      std::shared_ptr<const Cert> clientCert = nullptr)
      : type(type),
        mode(mode),
        serverCert(serverCert),
        clientCert(clientCert) {}
};
} // namespace

static NegotiatedPsk negotiatePsk(
    const std::vector<PskKeyExchangeMode>& supportedPskModes,
    const folly::Optional<CachedPsk>& attemptedPsk,
    const ServerHello& shlo,
    ProtocolVersion version,
    CipherSuite cipher,
    bool hasExchange) {
  auto serverPsk = getExtension<ServerPresharedKey>(shlo.extensions);
  if (!attemptedPsk) {
    if (serverPsk) {
      throw FizzException(
          "server accepted unattempted psk",
          AlertDescription::illegal_parameter);
    } else if (!supportedPskModes.empty()) {
      return NegotiatedPsk(PskType::NotAttempted);
    } else {
      return NegotiatedPsk(PskType::NotSupported);
    }
  } else {
    if (!serverPsk) {
      return NegotiatedPsk(PskType::Rejected);
    }
    if (serverPsk->selected_identity != 0) {
      throw FizzException(
          "server accepted non-0 psk", AlertDescription::illegal_parameter);
    }

    if (version != attemptedPsk->version) {
      throw FizzException(
          "different version in psk", AlertDescription::handshake_failure);
    }
    if (getHashFunction(cipher) != getHashFunction(attemptedPsk->cipher)) {
      throw FizzException(
          "incompatible cipher in psk", AlertDescription::handshake_failure);
    }

    PskKeyExchangeMode mode = hasExchange ? PskKeyExchangeMode::psk_dhe_ke
                                          : PskKeyExchangeMode::psk_ke;
    if (std::find(supportedPskModes.begin(), supportedPskModes.end(), mode) ==
        supportedPskModes.end()) {
      throw FizzException(
          "server choose unsupported psk mode",
          AlertDescription::handshake_failure);
    }

    return NegotiatedPsk(
        attemptedPsk->type,
        mode,
        attemptedPsk->serverCert,
        attemptedPsk->clientCert);
  }
}

Actions
EventHandler<ClientTypes, StateEnum::ExpectingServerHello, Event::ServerHello>::
    handle(const State& state, Param param) {
  auto shlo = std::move(*param.asServerHello());

  Protocol::checkAllowedExtensions(shlo, *state.requestedExtensions());

  ProtocolVersion version;
  CipherSuite cipher;
  Optional<std::tuple<NamedGroup, Buf, const KeyExchange*>> exchange;
  std::tie(version, cipher, exchange) = negotiateParameters(
      shlo,
      state.context()->getSupportedVersions(),
      state.context()->getSupportedCiphers(),
      *state.keyExchangers());

  if (!folly::IOBufEqualTo()(
          state.legacySessionId(), shlo.legacy_session_id_echo)) {
    throw FizzException(
        "session id echo mismatch", AlertDescription::illegal_parameter);
  }

  validateNegotiationConsistency(state, version, cipher);

  auto negotiatedPsk = negotiatePsk(
      state.context()->getSupportedPskModes(),
      state.attemptedPsk(),
      shlo,
      version,
      cipher,
      exchange.has_value());

  if (!exchange &&
      !(negotiatedPsk.mode &&
        *negotiatedPsk.mode == PskKeyExchangeMode::psk_ke)) {
    throw FizzException(
        "server did not send share", AlertDescription::handshake_failure);
  }

  std::unique_ptr<HandshakeContext> handshakeContext;
  if (state.handshakeContext()) {
    handshakeContext = std::move(state.handshakeContext());
  } else {
    handshakeContext =
        state.context()->getFactory()->makeHandshakeContext(cipher);
    handshakeContext->appendToTranscript(state.encodedClientHello());
  }
  handshakeContext->appendToTranscript(*shlo.originalEncoding);

  auto scheduler = state.context()->getFactory()->makeKeyScheduler(cipher);

  if (negotiatedPsk.mode) {
    scheduler->deriveEarlySecret(folly::range(state.attemptedPsk()->secret));
  }

  Optional<NamedGroup> group;
  Optional<KeyExchangeType> keyExchangeType;
  if (exchange) {
    if (state.keyExchangeType().has_value()) {
      keyExchangeType = *state.keyExchangeType();
    } else {
      keyExchangeType = KeyExchangeType::OneRtt;
    }

    Buf serverShare;
    const KeyExchange* kex;
    std::tie(group, serverShare, kex) = std::move(*exchange);
    auto sharedSecret = kex->generateSharedSecret(serverShare->coalesce());
    scheduler->deriveHandshakeSecret(sharedSecret->coalesce());
  } else {
    keyExchangeType = KeyExchangeType::None;
    scheduler->deriveHandshakeSecret();
  }

  if (state.readRecordLayer()->hasUnparsedHandshakeData()) {
    throw FizzException(
        "data after server hello", AlertDescription::unexpected_message);
  }

  auto handshakeWriteRecordLayer =
      state.context()->getFactory()->makeEncryptedWriteRecordLayer(
          EncryptionLevel::Handshake);
  handshakeWriteRecordLayer->setProtocolVersion(version);
  auto handshakeWriteSecret = scheduler->getSecret(
      HandshakeSecrets::ClientHandshakeTraffic,
      handshakeContext->getHandshakeContext()->coalesce());
  Protocol::setAead(
      *handshakeWriteRecordLayer,
      cipher,
      folly::range(handshakeWriteSecret.secret),
      *state.context()->getFactory(),
      *scheduler);

  auto handshakeReadRecordLayer =
      state.context()->getFactory()->makeEncryptedReadRecordLayer(
          EncryptionLevel::Handshake);
  handshakeReadRecordLayer->setProtocolVersion(version);
  auto handshakeReadSecret = scheduler->getSecret(
      HandshakeSecrets::ServerHandshakeTraffic,
      handshakeContext->getHandshakeContext()->coalesce());
  Protocol::setAead(
      *handshakeReadRecordLayer,
      cipher,
      folly::range(handshakeReadSecret.secret),
      *state.context()->getFactory(),
      *scheduler);

  auto clientHandshakeSecret =
      folly::IOBuf::copyBuffer(folly::range(handshakeWriteSecret.secret));
  auto serverHandshakeSecret =
      folly::IOBuf::copyBuffer(folly::range(handshakeReadSecret.secret));

  folly::Optional<ClientAuthType> authType;
  if (negotiatedPsk.clientCert) {
    authType = ClientAuthType::Stored;
  } else if (negotiatedPsk.serverCert) {
    authType = ClientAuthType::NotRequested;
  }

  std::chrono::system_clock::time_point handshakeTime;
  if (negotiatedPsk.mode) {
    handshakeTime = state.attemptedPsk()->ticketHandshakeTime;
  } else {
    handshakeTime = state.context()->getClock()->getCurrentTime();
  }

  return actions(
      MutateState(
          [keyScheduler = std::move(scheduler),
           readRecordLayer = std::move(handshakeReadRecordLayer),
           writeRecordLayer = std::move(handshakeWriteRecordLayer),
           handshakeContext = std::move(handshakeContext),
           version,
           cipher,
           group,
           clientHandshakeSecret = std::move(clientHandshakeSecret),
           serverHandshakeSecret = std::move(serverHandshakeSecret),
           keyExchangeType,
           pskType = negotiatedPsk.type,
           pskMode = negotiatedPsk.mode,
           serverCert = std::move(negotiatedPsk.serverCert),
           clientCert = std::move(negotiatedPsk.clientCert),
           authType = std::move(authType),
           handshakeTime = std::move(handshakeTime)](State& newState) mutable {
            newState.keyScheduler() = std::move(keyScheduler);
            newState.readRecordLayer() = std::move(readRecordLayer);
            newState.writeRecordLayer() = std::move(writeRecordLayer);
            newState.handshakeContext() = std::move(handshakeContext);
            newState.version() = version;
            newState.cipher() = cipher;
            newState.group() = group;
            newState.encodedClientHello() = folly::none;
            newState.keyExchangers() = folly::none;
            newState.attemptedPsk() = folly::none;
            newState.clientHandshakeSecret() = std::move(clientHandshakeSecret);
            newState.serverHandshakeSecret() = std::move(serverHandshakeSecret);
            newState.keyExchangeType() = keyExchangeType;
            newState.pskType() = pskType;
            newState.pskMode() = pskMode;
            newState.serverCert() = std::move(serverCert);
            newState.clientCert() = std::move(clientCert);
            newState.clientAuthRequested() = std::move(authType);
            newState.handshakeTime() = std::move(handshakeTime);
          }),
      SecretAvailable(std::move(handshakeReadSecret)),
      SecretAvailable(std::move(handshakeWriteSecret)),
      MutateState(&Transition<StateEnum::ExpectingEncryptedExtensions>));
}

namespace {
struct HrrParams {
  ProtocolVersion version;
  CipherSuite cipher;
  Optional<NamedGroup> group;
};
} // namespace

static HrrParams negotiateParameters(
    const HelloRetryRequest& hrr,
    const std::vector<ProtocolVersion>& supportedVersions,
    const std::vector<CipherSuite>& supportedCiphers,
    const std::vector<NamedGroup>& supportedGroups) {
  HrrParams negotiated;
  std::tie(negotiated.version, negotiated.cipher) =
      getAndValidateVersionAndCipher(hrr, supportedVersions, supportedCiphers);

  auto keyShare = getExtension<HelloRetryRequestKeyShare>(hrr.extensions);
  if (keyShare) {
    if (std::find(
            supportedGroups.begin(),
            supportedGroups.end(),
            keyShare->selected_group) == supportedGroups.end()) {
      throw FizzException(
          "server choose unsupported group in hrr",
          AlertDescription::handshake_failure);
    }
    negotiated.group = keyShare->selected_group;
  }

  return negotiated;
}

static std::map<NamedGroup, std::unique_ptr<KeyExchange>> getHrrKeyExchangers(
    const Factory& factory,
    std::map<NamedGroup, std::unique_ptr<KeyExchange>> previous,
    Optional<NamedGroup> negotiatedGroup) {
  if (negotiatedGroup) {
    if (previous.find(*negotiatedGroup) != previous.end()) {
      throw FizzException(
          "hrr selected already-sent group",
          AlertDescription::illegal_parameter);
    }
    return getKeyExchangers(factory, {*negotiatedGroup});
  } else {
    return previous;
  }
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingServerHello,
    Event::HelloRetryRequest>::handle(const State& state, Param param) {
  auto hrr = std::move(*param.asHelloRetryRequest());

  Protocol::checkAllowedExtensions(hrr);

  if (state.keyExchangeType().has_value()) {
    throw FizzException("two HRRs", AlertDescription::unexpected_message);
  }

  auto negotiatedParams = negotiateParameters(
      hrr,
      state.context()->getSupportedVersions(),
      state.context()->getSupportedCiphers(),
      state.context()->getSupportedGroups());

  ProtocolVersion version = negotiatedParams.version;
  CipherSuite cipher = negotiatedParams.cipher;
  Optional<NamedGroup> group = negotiatedParams.group;

  auto cookie = getExtension<Cookie>(hrr.extensions);

  auto attemptedPsk = state.attemptedPsk();
  if (attemptedPsk &&
      getHashFunction(attemptedPsk->cipher) != getHashFunction(cipher)) {
    attemptedPsk = folly::none;
  }

  // We move the current key exchangers in so getHrrKeyExchangers can either
  // return the current set with ownership or create a new one.
  auto keyExchangers = getHrrKeyExchangers(
      *state.context()->getFactory(), std::move(*state.keyExchangers()), group);

  auto chlo = getClientHello(
      *state.context()->getFactory(),
      state.clientRandom(),
      state.context()->getSupportedCiphers(),
      state.context()->getSupportedVersions(),
      state.context()->getSupportedGroups(),
      keyExchangers,
      state.context()->getSupportedSigSchemes(),
      state.context()->getSupportedPskModes(),
      state.sni(),
      state.context()->getSupportedAlpns(),
      state.context()->getSupportedCertDecompressionAlgorithms(),
      folly::none,
      state.legacySessionId(),
      state.extensions(),
      cookie ? std::move(cookie->cookie) : nullptr);

  auto firstHandshakeContext =
      state.context()->getFactory()->makeHandshakeContext(cipher);
  firstHandshakeContext->appendToTranscript(state.encodedClientHello());

  message_hash chloHash;
  chloHash.hash = firstHandshakeContext->getHandshakeContext();

  auto handshakeContext =
      state.context()->getFactory()->makeHandshakeContext(cipher);
  handshakeContext->appendToTranscript(encodeHandshake(std::move(chloHash)));
  handshakeContext->appendToTranscript(*hrr.originalEncoding);

  std::vector<ExtensionType> requestedExtensions;
  for (const auto& extension : chlo.extensions) {
    requestedExtensions.push_back(extension.extension_type);
  }

  Buf encodedClientHello;
  if (attemptedPsk) {
    requestedExtensions.push_back(ExtensionType::pre_shared_key);
    auto keyScheduler = state.context()->getFactory()->makeKeyScheduler(cipher);

    encodedClientHello = encodeAndAddBinders(
        std::move(chlo),
        *attemptedPsk,
        *keyScheduler,
        *handshakeContext,
        *state.context()->getClock());
  } else {
    encodedClientHello = encodeHandshake(std::move(chlo));
    handshakeContext->appendToTranscript(encodedClientHello);
  }

  auto earlyDataType = state.earlyDataType() == EarlyDataType::Attempted
      ? EarlyDataType::Rejected
      : state.earlyDataType();

  WriteToSocket clientFlight;
  auto chloWrite =
      state.writeRecordLayer()->writeHandshake(encodedClientHello->clone());

  bool sentCCS = state.sentCCS();
  folly::Optional<client::Action> ccsWrite;
  if (state.context()->getCompatibilityMode() && !sentCCS) {
    TLSContent writeCCS;
    writeCCS.data = folly::IOBuf::wrapBuffer(FakeChangeCipherSpec);
    writeCCS.contentType = ContentType::change_cipher_spec;
    writeCCS.encryptionLevel = EncryptionLevel::Plaintext;
    clientFlight.contents.emplace_back(std::move(writeCCS));
    sentCCS = true;
  }
  clientFlight.contents.emplace_back(std::move(chloWrite));

  return actions(
      MutateState([version,
                   cipher,
                   earlyDataType,
                   encodedClientHello = std::move(encodedClientHello),
                   keyExchangers = std::move(keyExchangers),
                   handshakeContext = std::move(handshakeContext),
                   attemptedPsk = std::move(attemptedPsk),
                   requestedExtensions = std::move(requestedExtensions),
                   sentCCS](State& newState) mutable {
        newState.version() = version;
        newState.cipher() = cipher;
        newState.earlyDataType() = earlyDataType;
        newState.earlyWriteRecordLayer() = nullptr;
        newState.encodedClientHello() = std::move(encodedClientHello);
        newState.keyExchangers() = std::move(keyExchangers);
        newState.handshakeContext() = std::move(handshakeContext);
        newState.keyExchangeType() = KeyExchangeType::HelloRetryRequest;
        newState.attemptedPsk() = std::move(attemptedPsk);
        newState.requestedExtensions() = std::move(requestedExtensions);
        newState.sentCCS() = sentCCS;
      }),
      std::move(clientFlight),
      MutateState(&Transition<StateEnum::ExpectingServerHello>));
}

static void validateAcceptedEarly(
    const State& state,
    const Optional<std::string>& alpn) {
  const auto& params = state.earlyDataParams();

  if (state.pskType() == PskType::Rejected || !params) {
    throw FizzException(
        "early accepted without psk", AlertDescription::illegal_parameter);
  }

  if (params->cipher != state.cipher()) {
    throw FizzException(
        "early accepted with different cipher",
        AlertDescription::illegal_parameter);
  }

  if (params->alpn != alpn) {
    throw FizzException(
        "early accepted with different alpn",
        AlertDescription::illegal_parameter);
  }

  if (!state.earlyWriteRecordLayer() &&
      !state.context()->getOmitEarlyRecordLayer()) {
    throw FizzException(
        "no early record layer", AlertDescription::illegal_parameter);
  }
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingEncryptedExtensions,
    Event::EncryptedExtensions>::handle(const State& state, Param param) {
  auto ee = std::move(*param.asEncryptedExtensions());

  Protocol::checkAllowedExtensions(ee, *state.requestedExtensions());

  state.handshakeContext()->appendToTranscript(*ee.originalEncoding);

  Optional<std::string> appProto;
  auto alpn = getExtension<ProtocolNameList>(ee.extensions);
  if (alpn) {
    if (alpn->protocol_name_list.size() != 1) {
      throw FizzException(
          "alpn list does not contain exactly one protocol",
          AlertDescription::illegal_parameter);
    }
    appProto =
        alpn->protocol_name_list.front().name->moveToFbString().toStdString();
    if (std::find(
            state.context()->getSupportedAlpns().begin(),
            state.context()->getSupportedAlpns().end(),
            *appProto) == state.context()->getSupportedAlpns().end()) {
      throw FizzException(
          folly::to<std::string>("alpn mismatch: server choose ", *appProto),
          AlertDescription::illegal_parameter);
    }
  } else if (state.context()->getRequireAlpn()) {
    throw FizzException(
        "alpn is required", AlertDescription::no_application_protocol);
  }

  auto serverEarly = getExtension<ServerEarlyData>(ee.extensions);
  auto earlyDataType = state.earlyDataType();
  if (state.earlyDataType() == EarlyDataType::Attempted) {
    if (serverEarly) {
      validateAcceptedEarly(state, appProto);
      earlyDataType = EarlyDataType::Accepted;
    } else {
      earlyDataType = EarlyDataType::Rejected;
    }
  } else {
    if (serverEarly) {
      throw FizzException(
          "unexpected accepted early data",
          AlertDescription::illegal_parameter);
    }
  }

  if (state.extensions()) {
    state.extensions()->onEncryptedExtensions(ee.extensions);
  }

  MutateState mutateState(
      [appProto = std::move(appProto), earlyDataType](State& newState) mutable {
        newState.alpn() = std::move(appProto);
        newState.requestedExtensions() = folly::none;
        newState.earlyDataType() = earlyDataType;
      });

  if (state.pskType() == PskType::Resumption ||
      state.pskType() == PskType::External) {
    return actions(
        std::move(mutateState),
        MutateState(&Transition<StateEnum::ExpectingFinished>));
  } else {
    return actions(
        std::move(mutateState),
        MutateState(&Transition<StateEnum::ExpectingCertificate>));
  }
}

static std::tuple<
    folly::Optional<SignatureScheme>,
    std::shared_ptr<const SelfCert>>
getClientCert(const State& state, const std::vector<SignatureScheme>& schemes) {
  folly::Optional<SignatureScheme> selectedScheme;
  auto clientCert = state.context()->getClientCertificate();
  const auto& supportedSchemes = state.context()->getSupportedSigSchemes();

  if (clientCert) {
    const auto certSchemes = clientCert->getSigSchemes();
    for (const auto& scheme : supportedSchemes) {
      if (std::find(certSchemes.begin(), certSchemes.end(), scheme) !=
              certSchemes.end() &&
          std::find(schemes.begin(), schemes.end(), scheme) != schemes.end()) {
        selectedScheme = scheme;
        break;
      }
    }

    if (!selectedScheme) {
      VLOG(1) << "client cert/context doesn't support any signature algorithms "
              << "specified by the server";
    }
  }

  if (!selectedScheme) {
    clientCert = nullptr;
  }

  return std::make_tuple(std::move(selectedScheme), std::move(clientCert));
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingCertificate,
    Event::CertificateRequest>::handle(const State& state, Param param) {
  if (state.clientAuthRequested()) {
    throw FizzException(
        "duplicate certificate request message",
        AlertDescription::unexpected_message);
  }

  auto certRequest = std::move(*param.asCertificateRequest());
  state.handshakeContext()->appendToTranscript(*certRequest.originalEncoding);

  if (!certRequest.certificate_request_context->empty()) {
    throw FizzException(
        "certificate request context must be empty",
        AlertDescription::illegal_parameter);
  }

  auto sigAlgsExtension =
      getExtension<SignatureAlgorithms>(certRequest.extensions);
  if (!sigAlgsExtension) {
    throw FizzException(
        "certificate request without signature algorithms",
        AlertDescription::illegal_parameter);
  }

  folly::Optional<SignatureScheme> scheme;
  std::shared_ptr<const SelfCert> cert;
  std::tie(scheme, cert) =
      getClientCert(state, sigAlgsExtension->supported_signature_algorithms);
  ClientAuthType authType =
      scheme ? ClientAuthType::Sent : ClientAuthType::RequestedNoMatch;

  MutateState mutateState([scheme = std::move(scheme),
                           cert = std::move(cert),
                           authType](State& newState) mutable {
    newState.clientAuthRequested() = authType;
    newState.selectedClientCert() = std::move(cert);
    newState.clientAuthSigScheme() = std::move(scheme);
  });

  return actions(
      std::move(mutateState),
      MutateState(&Transition<StateEnum::ExpectingCertificate>));
}

static MutateState handleCertMsg(
    const State& state,
    CertificateMsg certMsg,
    folly::Optional<CertificateCompressionAlgorithm> algo) {
  if (!certMsg.certificate_request_context->empty()) {
    throw FizzException(
        "certificate request context must be empty",
        AlertDescription::illegal_parameter);
  }

  std::vector<std::shared_ptr<const PeerCert>> serverCerts;
  bool leaf = true;
  for (auto& certEntry : certMsg.certificate_list) {
    if (state.extensions()) {
      // Check that these extensions correspond to ones we requested.
      auto sentExtensions = state.extensions()->getClientHelloExtensions();
      for (auto& ext : certEntry.extensions) {
        auto extIt = std::find_if(
            sentExtensions.begin(),
            sentExtensions.end(),
            [type = ext.extension_type](const Extension& sentExt) {
              return sentExt.extension_type == type;
            });
        if (extIt == sentExtensions.end()) {
          throw FizzException(
              "unrequested certificate extension:" +
                  toString(ext.extension_type),
              AlertDescription::illegal_parameter);
        }
      }
    } else {
      if (!certEntry.extensions.empty()) {
        throw FizzException(
            "certificate extensions must be empty",
            AlertDescription::illegal_parameter);
      }
    }

    serverCerts.emplace_back(state.context()->getFactory()->makePeerCert(
        std::move(certEntry), leaf));
    leaf = false;
  }

  if (serverCerts.empty()) {
    throw FizzException(
        "no certificates received", AlertDescription::illegal_parameter);
  }

  ClientAuthType authType =
      state.clientAuthRequested().value_or(ClientAuthType::NotRequested);

  return [unverifiedCertChain = std::move(serverCerts),
          authType,
          compAlgo = std::move(algo)](State& newState) mutable {
    newState.unverifiedCertChain() = std::move(unverifiedCertChain);
    newState.clientAuthRequested() = authType;
    newState.serverCertCompAlgo() = std::move(compAlgo);
  };
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingCertificate,
    Event::CompressedCertificate>::handle(const State& state, Param param) {
  if (state.context()->getSupportedCertDecompressionAlgorithms().empty()) {
    throw FizzException(
        "compressed certificate received unexpectedly",
        AlertDescription::unexpected_message);
  }

  auto compCert = std::move(*param.asCompressedCertificate());
  state.handshakeContext()->appendToTranscript(*compCert.originalEncoding);

  auto algos = state.context()->getSupportedCertDecompressionAlgorithms();
  if (std::find(algos.begin(), algos.end(), compCert.algorithm) ==
      algos.end()) {
    throw FizzException(
        "certificate compressed with unsupported algorithm: " +
            toString(compCert.algorithm),
        AlertDescription::bad_certificate);
  }

  auto decompressor =
      state.context()->getCertDecompressorForAlgorithm(compCert.algorithm);
  DCHECK(decompressor);

  CertificateMsg msg;
  try {
    msg = decompressor->decompress(compCert);
  } catch (const std::exception& e) {
    throw FizzException(
        folly::to<std::string>("certificate decompression failed: ", e.what()),
        AlertDescription::bad_certificate);
  }

  return actions(
      handleCertMsg(state, std::move(msg), compCert.algorithm),
      MutateState(&Transition<StateEnum::ExpectingCertificateVerify>));
}

Actions
EventHandler<ClientTypes, StateEnum::ExpectingCertificate, Event::Certificate>::
    handle(const State& state, Param param) {
  auto certMsg = std::move(*param.asCertificateMsg());

  state.handshakeContext()->appendToTranscript(*certMsg.originalEncoding);

  return actions(
      handleCertMsg(state, std::move(certMsg), folly::none),
      MutateState(&Transition<StateEnum::ExpectingCertificateVerify>));
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingCertificateVerify,
    Event::CertificateVerify>::handle(const State& state, Param param) {
  auto certVerify = std::move(*param.asCertificateVerify());

  if (std::find(
          state.context()->getSupportedSigSchemes().begin(),
          state.context()->getSupportedSigSchemes().end(),
          certVerify.algorithm) ==
      state.context()->getSupportedSigSchemes().end()) {
    throw FizzException(
        folly::to<std::string>(
            "server choose unsupported sig scheme: ",
            toString(certVerify.algorithm)),
        AlertDescription::illegal_parameter);
  }

  CHECK(!state.unverifiedCertChain().empty());
  auto leaf = state.unverifiedCertChain().front();

  leaf->verify(
      certVerify.algorithm,
      CertificateVerifyContext::Server,
      state.handshakeContext()->getHandshakeContext()->coalesce(),
      certVerify.signature->coalesce());

  if (state.verifier()) {
    try {
      state.verifier()->verify(state.unverifiedCertChain());
    } catch (const FizzException&) {
      std::rethrow_exception(std::current_exception());
    } catch (const std::exception& e) {
      throw FizzVerificationException(
          folly::to<std::string>("verifier failure: ", e.what()),
          AlertDescription::bad_certificate);
    }
  }

  state.handshakeContext()->appendToTranscript(*certVerify.originalEncoding);

  return actions(
      MutateState([sigScheme = certVerify.algorithm,
                   serverCert = std::move(leaf)](State& newState) mutable {
        newState.sigScheme() = sigScheme;
        newState.serverCert() = std::move(serverCert);
        newState.unverifiedCertChain() = folly::none;
      }),
      MutateState(&Transition<StateEnum::ExpectingFinished>));
}

Actions
EventHandler<ClientTypes, StateEnum::ExpectingFinished, Event::Finished>::
    handle(const State& state, Param param) {
  auto finished = std::move(*param.asFinished());

  if (state.readRecordLayer()->hasUnparsedHandshakeData()) {
    throw FizzException(
        "data after finished", AlertDescription::unexpected_message);
  }

  auto expectedFinished = state.handshakeContext()->getFinishedData(
      state.serverHandshakeSecret()->coalesce());
  if (!CryptoUtils::equal(
          expectedFinished->coalesce(), finished.verify_data->coalesce())) {
    throw FizzException(
        "server finished verify failure", AlertDescription::bad_record_mac);
  }

  state.handshakeContext()->appendToTranscript(*finished.originalEncoding);
  auto clientFinishedContext = state.handshakeContext()->getHandshakeContext();
  state.keyScheduler()->deriveMasterSecret();

  folly::Optional<TLSContent> eoedWrite;
  if (state.earlyDataType() == EarlyDataType::Accepted &&
      !state.context()->getOmitEarlyRecordLayer()) {
    auto encodedEndOfEarly = encodeHandshake(EndOfEarlyData());
    state.handshakeContext()->appendToTranscript(encodedEndOfEarly);
    DCHECK(state.earlyWriteRecordLayer());
    eoedWrite = state.earlyWriteRecordLayer()->writeHandshake(
        std::move(encodedEndOfEarly));
  }

  folly::Optional<Buf> encodedCertMessage;
  folly::Optional<Buf> encodedCertVerify;
  auto auth = *state.clientAuthRequested();
  std::shared_ptr<const Cert> clientCert;
  switch (auth) {
    case ClientAuthType::Stored:
      clientCert = state.clientCert();
      break;
    case ClientAuthType::RequestedNoMatch:
      encodedCertMessage = encodeHandshake(CertificateMsg());
      state.handshakeContext()->appendToTranscript(*encodedCertMessage);
      break;
    case ClientAuthType::Sent: {
      auto selectedCert = state.selectedClientCert();
      encodedCertMessage = encodeHandshake(selectedCert->getCertMessage());
      state.handshakeContext()->appendToTranscript(*encodedCertMessage);

      auto sigScheme = *state.clientAuthSigScheme();
      auto toSign = state.handshakeContext()->getHandshakeContext();
      auto signature = selectedCert->sign(
          sigScheme, CertificateVerifyContext::Client, toSign->coalesce());

      CertificateVerify verify;
      verify.algorithm = sigScheme;
      verify.signature = std::move(signature);
      encodedCertVerify = encodeHandshake(std::move(verify));
      state.handshakeContext()->appendToTranscript(*encodedCertVerify);

      clientCert = selectedCert;
      break;
    }
    case ClientAuthType::NotRequested:
      break;
  }

  auto exporterMasterVector = state.keyScheduler()->getSecret(
      MasterSecrets::ExporterMaster, clientFinishedContext->coalesce());
  auto exporterMaster =
      folly::IOBuf::copyBuffer(folly::range(exporterMasterVector.secret));

  auto encodedFinished = Protocol::getFinished(
      state.clientHandshakeSecret()->coalesce(), *state.handshakeContext());
  auto resumptionSecret = folly::IOBuf::copyBuffer(folly::range(
      state.keyScheduler()
          ->getSecret(
              MasterSecrets::ResumptionMaster,
              state.handshakeContext()->getHandshakeContext()->coalesce())
          .secret));

  WriteToSocket clientFlight;

  bool sentCCS = state.sentCCS();
  if (state.context()->getCompatibilityMode() && !sentCCS) {
    TLSContent writeCCS;
    writeCCS.encryptionLevel = EncryptionLevel::Plaintext;
    writeCCS.contentType = ContentType::change_cipher_spec;
    writeCCS.data = folly::IOBuf::wrapBuffer(FakeChangeCipherSpec);
    clientFlight.contents.emplace_back(std::move(writeCCS));
    sentCCS = true;
  }

  if (eoedWrite) {
    clientFlight.contents.emplace_back(std::move(*eoedWrite));
  }

  if (auth == ClientAuthType::RequestedNoMatch) {
    clientFlight.contents.emplace_back(state.writeRecordLayer()->writeHandshake(
        std::move(*encodedCertMessage), std::move(encodedFinished)));
  } else if (auth == ClientAuthType::Sent) {
    clientFlight.contents.emplace_back(state.writeRecordLayer()->writeHandshake(
        std::move(*encodedCertMessage),
        std::move(*encodedCertVerify),
        std::move(encodedFinished)));
  } else {
    clientFlight.contents.emplace_back(
        state.writeRecordLayer()->writeHandshake(std::move(encodedFinished)));
  }

  state.keyScheduler()->deriveAppTrafficSecrets(
      clientFinishedContext->coalesce());
  state.keyScheduler()->clearMasterSecret();

  auto writeRecordLayer =
      state.context()->getFactory()->makeEncryptedWriteRecordLayer(
          EncryptionLevel::AppTraffic);
  writeRecordLayer->setProtocolVersion(*state.version());
  auto writeSecret =
      state.keyScheduler()->getSecret(AppTrafficSecrets::ClientAppTraffic);
  Protocol::setAead(
      *writeRecordLayer,
      *state.cipher(),
      folly::range(writeSecret.secret),
      *state.context()->getFactory(),
      *state.keyScheduler());

  auto readRecordLayer =
      state.context()->getFactory()->makeEncryptedReadRecordLayer(
          EncryptionLevel::AppTraffic);
  readRecordLayer->setProtocolVersion(*state.version());
  auto readSecret =
      state.keyScheduler()->getSecret(AppTrafficSecrets::ServerAppTraffic);
  Protocol::setAead(
      *readRecordLayer,
      *state.cipher(),
      folly::range(readSecret.secret),
      *state.context()->getFactory(),
      *state.keyScheduler());

  ReportHandshakeSuccess reportSuccess;
  reportSuccess.earlyDataAccepted =
      state.earlyDataType() == EarlyDataType::Accepted;

  return actions(
      MutateState([readRecordLayer = std::move(readRecordLayer),
                   writeRecordLayer = std::move(writeRecordLayer),
                   resumptionSecret = std::move(resumptionSecret),
                   exporterMaster = std::move(exporterMaster),
                   clientCert = std::move(clientCert),
                   sentCCS](State& newState) mutable {
        newState.readRecordLayer() = std::move(readRecordLayer);
        newState.writeRecordLayer() = std::move(writeRecordLayer);
        newState.earlyWriteRecordLayer() = nullptr;
        newState.clientHandshakeSecret() = folly::none;
        newState.serverHandshakeSecret() = folly::none;
        newState.resumptionSecret() = std::move(resumptionSecret);
        newState.exporterMasterSecret() = std::move(exporterMaster);
        newState.selectedClientCert() = nullptr;
        newState.clientCert() = std::move(clientCert);
        newState.sentCCS() = sentCCS;
      }),
      MutateState(&Transition<StateEnum::Established>),
      SecretAvailable(std::move(readSecret)),
      SecretAvailable(std::move(writeSecret)),
      std::move(clientFlight),
      std::move(reportSuccess));
}

static uint32_t getMaxEarlyDataSize(const NewSessionTicket& nst) {
  auto earlyData = getExtension<TicketEarlyData>(nst.extensions);
  if (earlyData) {
    return earlyData->max_early_data_size;
  } else {
    return 0;
  }
}

Actions
EventHandler<ClientTypes, StateEnum::Established, Event::NewSessionTicket>::
    handle(const State& state, Param param) {
  auto nst = std::move(*param.asNewSessionTicket());

  auto derivedResumptionSecret = state.keyScheduler()->getResumptionSecret(
      state.resumptionSecret()->coalesce(), nst.ticket_nonce->coalesce());

  auto pskRange = nst.ticket->coalesce();
  auto secretRange = derivedResumptionSecret->coalesce();

  NewCachedPsk newCachedPsk;
  newCachedPsk.psk.psk = std::string(pskRange.begin(), pskRange.end());
  newCachedPsk.psk.secret = std::string(secretRange.begin(), secretRange.end());
  newCachedPsk.psk.type = PskType::Resumption;
  newCachedPsk.psk.version = *state.version();
  newCachedPsk.psk.cipher = *state.cipher();
  newCachedPsk.psk.group = state.group();
  newCachedPsk.psk.serverCert = state.serverCert();
  newCachedPsk.psk.clientCert = state.clientCert();
  newCachedPsk.psk.alpn = state.alpn();
  newCachedPsk.psk.ticketAgeAdd = nst.ticket_age_add;
  newCachedPsk.psk.ticketIssueTime =
      state.context()->getClock()->getCurrentTime();
  newCachedPsk.psk.ticketExpirationTime =
      state.context()->getClock()->getCurrentTime() +
      std::chrono::seconds(nst.ticket_lifetime);
  newCachedPsk.psk.ticketHandshakeTime = *state.handshakeTime();
  newCachedPsk.psk.maxEarlyDataSize = getMaxEarlyDataSize(nst);

  return actions(std::move(newCachedPsk));
}

Actions
EventHandler<ClientTypes, StateEnum::Established, Event::AppData>::handle(
    const State&,
    Param param) {
  auto& appData = *param.asAppData();

  return actions(DeliverAppData{std::move(appData.data)});
}

Actions
EventHandler<ClientTypes, StateEnum::Established, Event::AppWrite>::handle(
    const State& state,
    Param param) {
  auto& appWrite = *param.asAppWrite();

  WriteToSocket write;
  write.callback = appWrite.callback;
  write.contents.emplace_back(
      state.writeRecordLayer()->writeAppData(std::move(appWrite.data)));
  write.flags = appWrite.flags;

  return actions(std::move(write));
}

Actions
EventHandler<ClientTypes, StateEnum::Established, Event::KeyUpdate>::handle(
    const State& state,
    Param param) {
  auto& keyUpdate = *param.asKeyUpdate();

  if (state.readRecordLayer()->hasUnparsedHandshakeData()) {
    throw FizzException(
        "data after key_update", AlertDescription::unexpected_message);
  }
  state.keyScheduler()->serverKeyUpdate();
  auto readRecordLayer =
      state.context()->getFactory()->makeEncryptedReadRecordLayer(
          EncryptionLevel::AppTraffic);
  readRecordLayer->setProtocolVersion(*state.version());
  auto readSecret =
      state.keyScheduler()->getSecret(AppTrafficSecrets::ServerAppTraffic);
  Protocol::setAead(
      *readRecordLayer,
      *state.cipher(),
      folly::range(readSecret.secret),
      *state.context()->getFactory(),
      *state.keyScheduler());

  if (keyUpdate.request_update == KeyUpdateRequest::update_not_requested) {
    return actions(MutateState(
        [rRecordLayer = std::move(readRecordLayer)](State& newState) mutable {
          newState.readRecordLayer() = std::move(rRecordLayer);
        }));
  }

  auto encodedKeyUpdated =
      Protocol::getKeyUpdated(KeyUpdateRequest::update_not_requested);
  WriteToSocket write;
  write.contents.emplace_back(
      state.writeRecordLayer()->writeHandshake(std::move(encodedKeyUpdated)));

  state.keyScheduler()->clientKeyUpdate();

  auto writeRecordLayer =
      state.context()->getFactory()->makeEncryptedWriteRecordLayer(
          EncryptionLevel::AppTraffic);
  writeRecordLayer->setProtocolVersion(*state.version());
  auto writeSecret =
      state.keyScheduler()->getSecret(AppTrafficSecrets::ClientAppTraffic);
  Protocol::setAead(
      *writeRecordLayer,
      *state.cipher(),
      folly::range(writeSecret.secret),
      *state.context()->getFactory(),
      *state.keyScheduler());

  return actions(
      MutateState([rRecordLayer = std::move(readRecordLayer),
                   wRecordLayer =
                       std::move(writeRecordLayer)](State& newState) mutable {
        newState.readRecordLayer() = std::move(rRecordLayer);
        newState.writeRecordLayer() = std::move(wRecordLayer);
      }),
      SecretAvailable(std::move(readSecret)),
      SecretAvailable(std::move(writeSecret)),
      std::move(write));
}

// If we get an early data write after early data has been rejected we won't
// bother writing the data out but we can't just throw away the data without
// invoking a method on the write callback. Since the proper write callback
// action to invoke depends on how the higher layer will react to rejected
// early data, we give the write back in a special action for the higher layer
// to handle.
static Actions ignoreEarlyAppWrite(const State& state, EarlyAppWrite write) {
  if (*state.earlyDataType() != EarlyDataType::Rejected) {
    throw FizzException("ignoring valid early write", folly::none);
  }

  ReportEarlyWriteFailed failedWrite;
  failedWrite.write = std::move(write);
  return actions(std::move(failedWrite));
}

static Actions handleEarlyAppWrite(const State& state, EarlyAppWrite appWrite) {
  if (state.context()->getOmitEarlyRecordLayer()) {
    throw FizzException("early app writes disabled", folly::none);
  }

  switch (*state.earlyDataType()) {
    case EarlyDataType::NotAttempted:
      throw FizzException("invalid early write", folly::none);
    case EarlyDataType::Rejected:
      return ignoreEarlyAppWrite(state, std::move(appWrite));
    case EarlyDataType::Attempted:
    case EarlyDataType::Accepted: {
      WriteToSocket write;
      write.callback = appWrite.callback;
      write.flags = appWrite.flags;
      auto appData =
          state.earlyWriteRecordLayer()->writeAppData(std::move(appWrite.data));

      if (!state.sentCCS() && state.context()->getCompatibilityMode()) {
        TLSContent writeCCS;
        writeCCS.data = folly::IOBuf::wrapBuffer(FakeChangeCipherSpec);
        writeCCS.contentType = ContentType::change_cipher_spec;
        writeCCS.encryptionLevel = EncryptionLevel::Plaintext;
        write.contents.emplace_back(std::move(writeCCS));
        write.contents.emplace_back(std::move(appData));
        return actions(
            MutateState([](State& newState) { newState.sentCCS() = true; }),
            std::move(write));
      } else {
        write.contents.emplace_back(std::move(appData));
        return actions(std::move(write));
      }
    }
  }
  LOG(FATAL) << "Bad EarlyDataType";
  folly::assume_unreachable();
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingServerHello,
    Event::EarlyAppWrite>::handle(const State& state, Param param) {
  return handleEarlyAppWrite(state, std::move(*param.asEarlyAppWrite()));
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingEncryptedExtensions,
    Event::EarlyAppWrite>::handle(const State& state, Param param) {
  return handleEarlyAppWrite(state, std::move(*param.asEarlyAppWrite()));
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingCertificate,
    Event::EarlyAppWrite>::handle(const State& state, Param param) {
  return ignoreEarlyAppWrite(state, std::move(*param.asEarlyAppWrite()));
}

Actions EventHandler<
    ClientTypes,
    StateEnum::ExpectingCertificateVerify,
    Event::EarlyAppWrite>::handle(const State& state, Param param) {
  return ignoreEarlyAppWrite(state, std::move(*param.asEarlyAppWrite()));
}

Actions
EventHandler<ClientTypes, StateEnum::ExpectingFinished, Event::EarlyAppWrite>::
    handle(const State& state, Param param) {
  return handleEarlyAppWrite(state, std::move(*param.asEarlyAppWrite()));
}

Actions
EventHandler<ClientTypes, StateEnum::Established, Event::EarlyAppWrite>::handle(
    const State& state,
    Param param) {
  auto appWrite = std::move(*param.asEarlyAppWrite());
  if (*state.earlyDataType() == EarlyDataType::Accepted) {
    // It's possible that we had queued early writes before full handshake
    // success. It's fine to write them on the normal record layer as long as
    // the early data was accepted, otherwise we need to ignore them to preserve
    // the all-or-nothing property of early data.
    WriteToSocket write;
    write.callback = appWrite.callback;
    write.contents.emplace_back(
        state.writeRecordLayer()->writeAppData(std::move(appWrite.data)));
    write.flags = appWrite.flags;
    return actions(std::move(write));
  } else {
    return ignoreEarlyAppWrite(state, std::move(appWrite));
  }
}

Actions
EventHandler<ClientTypes, StateEnum::Established, Event::CloseNotify>::handle(
    const State& state,
    Param param) {
  ensureNoUnparsedHandshakeData(state, Event::CloseNotify);
  auto& closenotify = *param.asCloseNotify();
  auto eod = EndOfData(std::move(closenotify.ignoredPostCloseData));

  MutateState clearRecordLayers = ([](State& newState) {
    newState.writeRecordLayer() = nullptr;
    newState.readRecordLayer() = nullptr;
  });

  WriteToSocket write;
  write.contents.emplace_back(state.writeRecordLayer()->writeAlert(
      Alert(AlertDescription::close_notify)));
  return actions(
      std::move(write),
      std::move(clearRecordLayers),
      MutateState(&Transition<StateEnum::Closed>),
      std::move(eod));
}

Actions
EventHandler<ClientTypes, StateEnum::ExpectingCloseNotify, Event::CloseNotify>::
    handle(const State& state, Param param) {
  ensureNoUnparsedHandshakeData(state, Event::CloseNotify);
  auto& closenotify = *param.asCloseNotify();
  auto eod = EndOfData(std::move(closenotify.ignoredPostCloseData));

  MutateState clearRecordLayers([](State& newState) {
    newState.readRecordLayer() = nullptr;
    newState.writeRecordLayer() = nullptr;
  });
  return actions(
      std::move(clearRecordLayers),
      MutateState(&Transition<StateEnum::Closed>),
      std::move(eod));
}

} // namespace sm
} // namespace fizz
