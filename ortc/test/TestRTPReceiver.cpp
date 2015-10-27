/*
 
 Copyright (c) 2015, Hookflash Inc.
 All rights reserved.
 
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 
 1. Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
 The views and conclusions contained in the software and documentation are those
 of the authors and should not be interpreted as representing official policies,
 either expressed or implied, of the FreeBSD Project.
 
 */


#include <zsLib/MessageQueueThread.h>

#include "TestRTPReceiver.h"

#include <ortc/internal/ortc_RTPPacket.h>
#include <ortc/internal/ortc_RTCPPacket.h>
#include <ortc/IRTPTypes.h>
#include <ortc/ISettings.h>

#include <zsLib/XML.h>

#include "config.h"
#include "testing.h"

namespace ortc { namespace test { ZS_DECLARE_SUBSYSTEM(ortc_test) } }

using zsLib::String;
using zsLib::ULONG;
using zsLib::PTRNUMBER;
using zsLib::IMessageQueue;
using zsLib::Log;
using zsLib::AutoPUID;
using zsLib::AutoRecursiveLock;
using namespace zsLib::XML;

ZS_DECLARE_TYPEDEF_PTR(ortc::ISettings, UseSettings)
ZS_DECLARE_TYPEDEF_PTR(openpeer::services::IHelper, UseServicesHelper)

namespace ortc
{
  namespace test
  {
    namespace rtpreceiver
    {
      ZS_DECLARE_CLASS_PTR(FakeICETransport)
      ZS_DECLARE_CLASS_PTR(FakeSecureTransport)
      ZS_DECLARE_CLASS_PTR(RTPReceiverTester)

      ZS_DECLARE_TYPEDEF_PTR(ortc::internal::ISecureTransportTypes, ISecureTransportTypes)
      ZS_DECLARE_TYPEDEF_PTR(ortc::internal::ISecureTransport, ISecureTransport)
      ZS_DECLARE_USING_PTR(ortc::internal, RTPReceiver)
      ZS_DECLARE_USING_PTR(ortc::internal, RTPReceiverChannel)
      ZS_DECLARE_USING_PTR(ortc::internal, RTPSender)
      ZS_DECLARE_USING_PTR(ortc::internal, RTPListener)
      ZS_DECLARE_USING_PTR(ortc::internal, MediaStreamTrack)

      using zsLib::AutoRecursiveLock;

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark (helpers)
      #pragma mark

      //-------------------------------------------------------------------------
      static bool isRTCPPacketType(const BYTE *data, size_t len)
      {
        if (len < 2) {
          return false;
        }
        BYTE pt = (data[1] & 0x7F);
        return (63 < pt) && (pt < 96);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeICETransport
      #pragma mark

      //-----------------------------------------------------------------------
      FakeICETransport::FakeICETransport(
                                         const make_private &,
                                         IMessageQueuePtr queue,
                                         Milliseconds packetDelay
                                         ) :
        ICETransport(zsLib::Noop(true), queue),
        mPacketDelay(packetDelay)
      {
        ZS_LOG_BASIC(log("created"))
      }

      //-----------------------------------------------------------------------
      void FakeICETransport::init()
      {
        AutoRecursiveLock lock(*this);
        if (Milliseconds() != mPacketDelay) {
          mTimer = Timer::create(mThisWeak.lock(), mPacketDelay);
        }
      }

      //-----------------------------------------------------------------------
      FakeICETransport::~FakeICETransport()
      {
        mThisWeak.reset();

        ZS_LOG_BASIC(log("destroyed"))
      }

      //-----------------------------------------------------------------------
      FakeICETransportPtr FakeICETransport::create(
                                                   IMessageQueuePtr queue,
                                                   Milliseconds packetDelay
                                                   )
      {
        FakeICETransportPtr pThis(make_shared<FakeICETransport>(make_private{}, queue, packetDelay));
        pThis->mThisWeak = pThis;
        pThis->init();
        return pThis;
      }

      //-----------------------------------------------------------------------
      void FakeICETransport::reliability(ULONG percentage)
      {
        AutoRecursiveLock lock(*this);
        mReliability = percentage;
      }

      //-----------------------------------------------------------------------
      void FakeICETransport::linkTransport(FakeICETransportPtr transport)
      {
        AutoRecursiveLock lock(*this);
        mLinkedTransport = transport;

        if (transport) {
          ZS_LOG_BASIC(log("transport linked") + ZS_PARAM("linked transport", transport->getID()))
        } else {
          ZS_LOG_BASIC(log("transport unlinked"))
        }
      }

      //-----------------------------------------------------------------------
      void FakeICETransport::state(IICETransport::States newState)
      {
        AutoRecursiveLock lock(*this);
        setState(newState);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeICETransport => IICETransport
      #pragma mark

      //-----------------------------------------------------------------------
      ElementPtr FakeICETransport::toDebug() const
      {
        AutoRecursiveLock lock(*this);

        ElementPtr resultEl = Element::create("ortc::test::rtpreceiver::FakeICETransport");

        UseServicesHelper::debugAppend(resultEl, "id", getID());

        UseServicesHelper::debugAppend(resultEl, "state", IICETransport::toString(mCurrentState));

        UseServicesHelper::debugAppend(resultEl, "secure transport id", mSecureTransportID);
        UseServicesHelper::debugAppend(resultEl, "secure transport", (bool)(mSecureTransport.lock()));

        UseServicesHelper::debugAppend(resultEl, "linked transport", (bool)(mLinkedTransport.lock()));

        UseServicesHelper::debugAppend(resultEl, "subscriptions", mSubscriptions.size());
        UseServicesHelper::debugAppend(resultEl, "default subscription", (bool)mDefaultSubscription);

        return resultEl;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeICETransport => IFakeICETransportAsyncDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeICETransport::onPacketFromLinkedFakedTransport(SecureByteBlockPtr buffer)
      {
        FakeSecureTransportPtr transport;

        {
          AutoRecursiveLock lock(*this);
          transport = mSecureTransport.lock();
          if (!transport) {
            ZS_LOG_WARNING(Detail, log("no rtpreceiver transport attached (thus cannot forward received packet)") + ZS_PARAM("buffer", (PTRNUMBER)(buffer->BytePtr())) + ZS_PARAM("buffer size", buffer->SizeInBytes()))
            return;
          }

          switch (state()) {
            case IICETransport::State_New:
            case IICETransport::State_Checking:
            case IICETransport::State_Disconnected:
            case IICETransport::State_Failed:
            case IICETransport::State_Closed:
            {
              ZS_LOG_WARNING(Detail, log("dropping incoming packet to simulate non-connected state"))
              return;
            }
            case IICETransport::State_Connected:
            case IICETransport::State_Completed:
            {
              break;
            }
          }
        }

        TESTING_CHECK(buffer)

        if (Milliseconds() != mPacketDelay) {
          ZS_LOG_DEBUG(log("delaying packet arrival") + ZS_PARAM("buffer", (PTRNUMBER)(buffer->BytePtr())) + ZS_PARAM("buffer size", buffer->SizeInBytes()))
          mDelayedBuffers.push_back(DelayedBufferPair(zsLib::now() + mPacketDelay, buffer));
          return;
        }

        ZS_LOG_DEBUG(log("packet received") + ZS_PARAM("buffer", (PTRNUMBER)(buffer->BytePtr())) + ZS_PARAM("buffer size", buffer->SizeInBytes()))

        transport->handleReceivedPacket(mComponent, buffer->BytePtr(), buffer->SizeInBytes());
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeICETransport => ITimerDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeICETransport::onTimer(TimerPtr timer)
      {
        FakeSecureTransportPtr transport;

        DelayedBufferList delayedPackets;

        auto tick = zsLib::now();

        {
          AutoRecursiveLock lock(*this);
          transport = mSecureTransport.lock();
          if (!transport) {
            ZS_LOG_WARNING(Detail, log("no rtpreceiver transport attached (thus cannot forward delayed packets)"))
            return;
          }

          while (mDelayedBuffers.size() > 0) {
            Time &delayTime = mDelayedBuffers.front().first;
            SecureByteBlockPtr buffer = mDelayedBuffers.front().second;

            if (delayTime > tick) {
              ZS_LOG_INSANE(log("delaying packet until tick") + ZS_PARAM("delay", delayTime) + ZS_PARAM("tick", tick))
              break;
            }

            switch (state()) {
              case IICETransport::State_New:
              case IICETransport::State_Checking:
              case IICETransport::State_Disconnected:
              case IICETransport::State_Failed:
              case IICETransport::State_Closed:
              {
                ZS_LOG_WARNING(Detail, log("dropping incoming packet to simulate non-connected state"))
                return;
              }
              case IICETransport::State_Connected:
              case IICETransport::State_Completed:
              {
                delayedPackets.push_back(DelayedBufferPair(delayTime, buffer));
                break;
              }
            }

            mDelayedBuffers.pop_front();
          }
        }

        for (auto iter = delayedPackets.begin(); iter != delayedPackets.end(); ++iter)
        {
          auto buffer = (*iter).second;

          ZS_LOG_DEBUG(log("packet received (after delay)") + ZS_PARAM("buffer", (PTRNUMBER)(buffer->BytePtr())) + ZS_PARAM("buffer size", buffer->SizeInBytes()))

          transport->handleReceivedPacket(mComponent, buffer->BytePtr(), buffer->SizeInBytes());
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeICETransport => friend FakeSecureTransport
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeICETransport::attachSecure(FakeSecureTransportPtr transport)
      {
        AutoRecursiveLock lock(*this);
        TESTING_CHECK(!((bool)mSecureTransport.lock()))
        mSecureTransportID = transport->getID();
        mSecureTransport = transport;
      }

      //-----------------------------------------------------------------------
      void FakeICETransport::detachSecure(FakeSecureTransport &transport)
      {
        AutoRecursiveLock lock(*this);
        if (transport.getID() != mSecureTransportID) return;
        mSecureTransportID = 0;
        mSecureTransport.reset();
      }

      //---------------------------------------------------------------------
      bool FakeICETransport::sendPacket(
                                        const BYTE *buffer,
                                        size_t bufferSizeInBytes
                                        )
      {
        FakeICETransportPtr transport;

        {
          AutoRecursiveLock lock(*this);
          transport = mLinkedTransport.lock();
          if (!transport) {
            ZS_LOG_WARNING(Detail, log("not linked to another fake transport") + ZS_PARAM("buffer", (PTRNUMBER)(buffer)) + ZS_PARAM("buffer size", bufferSizeInBytes))
            return false;
          }

          switch (state()) {
            case IICETransport::State_New:
            case IICETransport::State_Checking:
            case IICETransport::State_Disconnected:
            case IICETransport::State_Failed:
            case IICETransport::State_Closed:
            {
              ZS_LOG_WARNING(Detail, log("dropping outgoing packet to simulate non-connected state") + ZS_PARAM("buffer size", bufferSizeInBytes))
              return false;
            }
            case IICETransport::State_Connected:
            case IICETransport::State_Completed:
            {
              break;
            }
          }

          if (100 != mReliability) {
            auto value = UseServicesHelper::random(0, 100);
            if (value >= mReliability) {
              ZS_LOG_WARNING(Trace, log("intentionally dropping packet due to connectivity failure") + ZS_PARAM("buffer size", bufferSizeInBytes))
              return true;
            }
          }
        }

        ZS_LOG_DEBUG(log("sending packet to linked fake transport") + ZS_PARAM("other transport", transport->getID()) + ZS_PARAM("buffer", (PTRNUMBER)(buffer)) + ZS_PARAM("buffer size", bufferSizeInBytes))

        SecureByteBlockPtr sendBuffer(make_shared<SecureByteBlock>(buffer, bufferSizeInBytes));

        IFakeICETransportAsyncDelegateProxy::create(transport)->onPacketFromLinkedFakedTransport(sendBuffer);
        return true;
      }

      //-----------------------------------------------------------------------
      IICETransportTypes::States FakeICETransport::state() const
      {
        return mCurrentState;
      }

      //-------------------------------------------------------------------------
      IICETransportSubscriptionPtr FakeICETransport::subscribe(IICETransportDelegatePtr originalDelegate)
      {
        ZS_LOG_DETAIL(log("subscribing to transport state"))

        AutoRecursiveLock lock(*this);
        if (!originalDelegate) return mDefaultSubscription;

        IICETransportSubscriptionPtr subscription = mSubscriptions.subscribe(originalDelegate, getAssociatedMessageQueue());

        IICETransportDelegatePtr delegate = mSubscriptions.delegate(subscription, true);

        if (delegate) {
          FakeICETransportPtr pThis = mThisWeak.lock();

          if (IICETransportTypes::State_New != mCurrentState) {
            delegate->onICETransportStateChanged(pThis, mCurrentState);
          }
        }

        return subscription;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeICETransport => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeICETransport::setState(IICETransportTypes::States state)
      {
        if (state == mCurrentState) return;

        ZS_LOG_DETAIL(log("state changed") + ZS_PARAM("new state", IICETransport::toString(state)) + ZS_PARAM("old state", IICETransport::toString(mCurrentState)))

        mCurrentState = state;

        auto pThis = mThisWeak.lock();
        if (pThis) {
          mSubscriptions.delegate()->onICETransportStateChanged(pThis, mCurrentState);
        }
      }

      //-----------------------------------------------------------------------
      bool FakeICETransport::isShutdown()
      {
        return IICETransport::State_Closed == mCurrentState;
      }

      //-----------------------------------------------------------------------
      Log::Params FakeICETransport::log(const char *message) const
      {
        ElementPtr objectEl = Element::create("ortc::test::rtpreceiver::FakeICETransport");
        UseServicesHelper::debugAppend(objectEl, "id", ICETransport::getID());
        return Log::Params(message, objectEl);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSecureTransport
      #pragma mark

      //-----------------------------------------------------------------------
      FakeSecureTransport::FakeSecureTransport(
                                               const make_private &,
                                               IMessageQueuePtr queue,
                                               FakeICETransportPtr iceTransport
                                               ) :
        DTLSTransport(zsLib::Noop(true), queue),
        mICETransport(iceTransport)
      {
        ZS_LOG_BASIC(log("created"))
      }

      //-----------------------------------------------------------------------
      void FakeSecureTransport::init()
      {
        AutoRecursiveLock lock(*this);
        mICETransport->attachSecure(mThisWeak.lock());

        mICESubscription = mICETransport->subscribe(mThisWeak.lock());

        mListener = UseListener::create(mThisWeak.lock());
      }

      //-----------------------------------------------------------------------
      FakeSecureTransport::~FakeSecureTransport()
      {
        mThisWeak.reset();

        ZS_LOG_BASIC(log("destroyed"))

        cancel();
      }

      //-----------------------------------------------------------------------
      FakeSecureTransportPtr FakeSecureTransport::create(
                                                         IMessageQueuePtr queue,
                                                         FakeICETransportPtr iceTransport
                                                         )
      {
        FakeSecureTransportPtr pThis(make_shared<FakeSecureTransport>(make_private{}, queue, iceTransport));
        pThis->mThisWeak = pThis;
        pThis->init();
        return pThis;
      }

      //-----------------------------------------------------------------------
      void FakeSecureTransport::state(IDTLSTransport::States newState)
      {
        AutoRecursiveLock lock(*this);
        setState(newState);
      }

      //-----------------------------------------------------------------------
      void FakeSecureTransport::setClientRole(bool clientRole)
      {
        AutoRecursiveLock lock(*this);
        mClientRole = clientRole;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSecureTransport => IICETransport
      #pragma mark

      //-----------------------------------------------------------------------
      ElementPtr FakeSecureTransport::toDebug() const
      {
        AutoRecursiveLock lock(*this);

        ElementPtr resultEl = Element::create("ortc::test::rtpreceiver::FakeICETransport");

        UseServicesHelper::debugAppend(resultEl, "id", getID());

        UseServicesHelper::debugAppend(resultEl, "state", IDTLSTransport::toString(mCurrentState));

        UseServicesHelper::debugAppend(resultEl, "ice transport", mICETransport ? mICETransport->getID() : 0);

        UseServicesHelper::debugAppend(resultEl, "subscriptions", mSubscriptions.size());
        UseServicesHelper::debugAppend(resultEl, "default subscription", (bool)mDefaultSubscription);

        return resultEl;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSecureTransport => ISecureTransportForRTPReceiver
      #pragma mark

      //-----------------------------------------------------------------------
      RTPListenerPtr FakeSecureTransport::getListener() const
      {
        return RTPListener::convert(mListener);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSecureTransport => ISecureTransportForRTPSender
      #pragma mark

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSecureTransport => ISecureTransportForRTPReceiver
      #pragma mark

      //-----------------------------------------------------------------------
      PUID FakeSecureTransport::getID() const
      {
        return DTLSTransport::getID();
      }

      //-----------------------------------------------------------------------
      FakeSecureTransport::ISecureTransportSubscriptionPtr FakeSecureTransport::subscribe(ISecureTransportDelegatePtr originalDelegate)
      {
        ZS_LOG_DETAIL(log("subscribing to transport state"))

        AutoRecursiveLock lock(*this);
        if (!originalDelegate) return mDefaultSubscription;

        ISecureTransportSubscriptionPtr subscription = mSubscriptions.subscribe(originalDelegate, getAssociatedMessageQueue());

        ISecureTransportDelegatePtr delegate = mSubscriptions.delegate(subscription, true);

        if (delegate) {
          FakeSecureTransportPtr pThis = mThisWeak.lock();

          if (ISecureTransportTypes::State_Pending != mLastReportedState) {
            delegate->onSecureTransportStateChanged(pThis, mLastReportedState);
          }
        }
        
        return subscription;
      }

      //-----------------------------------------------------------------------
      ISecureTransportTypes::States FakeSecureTransport::state(ISecureTransportTypes::States ignored) const
      {
        return getSecureState();
      }

      //-----------------------------------------------------------------------
      bool FakeSecureTransport::sendPacket(
                                           IICETypes::Components sendOverICETransport,
                                           IICETypes::Components packetType,
                                           const BYTE *buffer,
                                           size_t bufferLengthInBytes
                                           )
      {
        FakeICETransportPtr iceTransport;

        {
          AutoRecursiveLock lock(*this);
          if (State_Validated != mCurrentState) {
            ZS_LOG_WARNING(Detail, log("cannot send packet when not in validated state"))
            return false;
          }

          iceTransport = mICETransport;
        }

        return iceTransport->sendPacket(buffer, bufferLengthInBytes);
      }

      //-----------------------------------------------------------------------
      IICETransportPtr FakeSecureTransport::getICETransport() const
      {
        return mICETransport;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSecureTransport => IICETransportDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeSecureTransport::onICETransportStateChanged(
                                                           IICETransportPtr transport,
                                                           IICETransport::States state
                                                           )
      {
        ZS_LOG_BASIC(log("ice transport state changed") + ZS_PARAM("transport", transport->getID()) + ZS_PARAM("state", IICETransportTypes::toString(state)))

        AutoRecursiveLock lock(*this);
      }

      //-----------------------------------------------------------------------
      void FakeSecureTransport::onICETransportCandidatePairAvailable(
                                                                     IICETransportPtr transport,
                                                                     CandidatePairPtr candidatePair
                                                                     )
      {
        // ignored
      }

      //-----------------------------------------------------------------------
      void FakeSecureTransport::onICETransportCandidatePairGone(
                                                                IICETransportPtr transport,
                                                                CandidatePairPtr candidatePair
                                                                )
      {
        // ignored
      }

      //-----------------------------------------------------------------------
      void FakeSecureTransport::onICETransportCandidatePairChanged(
                                                                   IICETransportPtr transport,
                                                                   CandidatePairPtr candidatePair
                                                                   )
      {
        // ignored
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSecureTransport => IFakeSecureTransportAsyncDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeICETransport => friend FakeICETransport
      #pragma mark

      //-----------------------------------------------------------------------
      bool FakeSecureTransport::handleReceivedPacket(
                                                     IICETypes::Components component,
                                                     const BYTE *buffer,
                                                     size_t bufferSizeInBytes
                                                     )
      {
        UseListenerPtr listener;

        {
          AutoRecursiveLock lock(*this);

          if (State_Validated != mCurrentState) {
            ZS_LOG_WARNING(Detail, log("dropping incoming packet to simulate non validated state"))
            return false;
          }

          listener = mListener;
        }

        if (!listener) {
          ZS_LOG_WARNING(Detail, log("dropping incoming packet (as data channel is gone)"))
          return false;
        }

        return listener->handleRTPPacket(component, isRTCPPacketType(buffer, bufferSizeInBytes) ? IICETypes::Component_RTCP : IICETypes::Component_RTP, buffer, bufferSizeInBytes);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSecureTransport => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeSecureTransport::setState(IDTLSTransportTypes::States state)
      {
        if (state == mCurrentState) return;

        ZS_LOG_DETAIL(log("state changed") + ZS_PARAM("new state", IDTLSTransport::toString(state)) + ZS_PARAM("old state", IDTLSTransport::toString(mCurrentState)))

        mCurrentState = state;
        notifySecureState();
      }

      //-----------------------------------------------------------------------
      bool FakeSecureTransport::isShutdown()
      {
        return IDTLSTransport::State_Closed == mCurrentState;
      }

      //-----------------------------------------------------------------------
      Log::Params FakeSecureTransport::log(const char *message) const
      {
        ElementPtr objectEl = Element::create("ortc::test::rtpreceiver::FakeSecureTransport");
        UseServicesHelper::debugAppend(objectEl, "id", DTLSTransport::getID());
        return Log::Params(message, objectEl);
      }

      //-----------------------------------------------------------------------
      void FakeSecureTransport::cancel()
      {
        setState(IDTLSTransport::State_Closed);

        mICETransport->detachSecure(*this);
      }

      //-----------------------------------------------------------------------
      IDTLSTransportTypes::States FakeSecureTransport::state() const
      {
        return mCurrentState;
      }

      //-----------------------------------------------------------------------
      void FakeSecureTransport::notifySecureState()
      {
        auto state = getSecureState();
        if (state == mLastReportedState) return;

        ZS_LOG_BASIC(log("notify state change") + ZS_PARAM("state", ISecureTransportTypes::toString(state)))

        auto pThis = mThisWeak.lock();
        if (pThis) {
          mSubscriptions.delegate()->onSecureTransportStateChanged(mThisWeak.lock(), state);
        }
        mLastReportedState = state;
      }

      //-----------------------------------------------------------------------
      ISecureTransportTypes::States FakeSecureTransport::getSecureState() const
      {
        if (!mICETransport) return ISecureTransportTypes::State_Closed;

        auto iceState = mICETransport->state();
        switch (iceState) {
          case IICETransportTypes::State_New:           return ISecureTransportTypes::State_Pending;
          case IICETransportTypes::State_Checking:      return ISecureTransportTypes::State_Pending;
          case IICETransportTypes::State_Connected:     break;
          case IICETransportTypes::State_Completed:     break;
          case IICETransportTypes::State_Disconnected:
          case IICETransportTypes::State_Failed:        {
            switch (state()) {
              case IDTLSTransportTypes::State_New:        return ISecureTransportTypes::State_Pending;
              case IDTLSTransportTypes::State_Connecting:
              case IDTLSTransportTypes::State_Connected:  return ISecureTransportTypes::State_Pending;
              case IDTLSTransportTypes::State_Validated:  return ISecureTransportTypes::State_Disconnected;
              case IDTLSTransportTypes::State_Closed:     return ISecureTransportTypes::State_Closed;
            }
            break;
          }
          case IICETransportTypes::State_Closed:        return ISecureTransportTypes::State_Closed;
        }

        switch (state()) {
          case IDTLSTransportTypes::State_New:        return ISecureTransportTypes::State_Pending;
          case IDTLSTransportTypes::State_Connecting: return ISecureTransportTypes::State_Pending;
          case IDTLSTransportTypes::State_Connected:  return ISecureTransportTypes::State_Pending;
          case IDTLSTransportTypes::State_Validated:  return ISecureTransportTypes::State_Connected;
          case IDTLSTransportTypes::State_Closed:     return ISecureTransportTypes::State_Closed;
        }

        return ISecureTransportTypes::State_Closed;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeListener
      #pragma mark

      //-----------------------------------------------------------------------
      FakeListener::FakeListener(IMessageQueuePtr queue) :
        RTPListener(Noop(true), queue)
      {
      }

      //-----------------------------------------------------------------------
      FakeListener::~FakeListener()
      {
        mThisWeak.reset();
        cancel();
      }

      //-----------------------------------------------------------------------
      FakeListenerPtr FakeListener::create(IMessageQueuePtr queue)
      {
        FakeListenerPtr pThis(make_shared<FakeListener>(queue));
        pThis->mThisWeak = pThis;
        pThis->init();
        return pThis;
      }

      //-----------------------------------------------------------------------
      FakeListenerPtr FakeListener::convert(RTPListenerPtr listener)
      {
        return ZS_DYNAMIC_PTR_CAST(FakeListener, listener);
      }

      //-----------------------------------------------------------------------
      void FakeListener::init()
      {
        AutoRecursiveLock lock(*this);

        mCleanBuffersTimer = Timer::create(mThisWeak.lock(), Seconds(5));
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeListener => IRTPReceiverForRTPReceiver
      #pragma mark

      //-----------------------------------------------------------------------
      ElementPtr FakeListener::toDebug() const
      {
        AutoRecursiveLock lock(*this);

        ElementPtr result = Element::create("ortc::test::rtpreceiver::FakeListener");

        UseServicesHelper::debugAppend(result, "tester", (bool)mTester.lock());

        UseServicesHelper::debugAppend(result, "subscriptions", mSubscriptions.size());
        UseServicesHelper::debugAppend(result, "default subscription", (bool)mDefaultSubscription);

        UseServicesHelper::debugAppend(result, "clean timer", mCleanBuffersTimer ? mCleanBuffersTimer->getID() : 0);

        UseServicesHelper::debugAppend(result, "buffered rtp packets", mBufferedRTPPackets.size());
        UseServicesHelper::debugAppend(result, "buffered rtcp packets", mBufferedRTCPPackets.size());

        auto receiver = mReceiver.lock();
        UseServicesHelper::debugAppend(result, "receiver", receiver ? receiver->getID() : 0);

        UseServicesHelper::debugAppend(result, "unhandled", mUnhandled.size());

        return result;
      }

      //-----------------------------------------------------------------------
      void FakeListener::registerReceiver(
                                          UseReceiverPtr inReceiver,
                                          const Parameters &inParams,
                                          RTCPPacketList *outPacketList
                                          )
      {
        ZS_LOG_BASIC(log("register receiver") + ZS_PARAM("receiver", inReceiver->getID()) + inParams.toDebug())

        AutoRecursiveLock lock(*this);
        mReceiver = inReceiver;

        if (outPacketList) {
          for (auto iter = mBufferedRTCPPackets.begin(); iter != mBufferedRTCPPackets.end(); ++iter) {
            auto &rtcpPacket = (*iter).second;
            outPacketList->push_back(rtcpPacket);
          }
        }

        for (auto iter = mBufferedRTPPackets.begin(); iter != mBufferedRTPPackets.end(); ++iter) {
          auto &rtpPacket = (*iter).second;
          IFakeListenerAsyncDelegateProxy::create(mThisWeak.lock())->onForwardBufferedPacket(rtpPacket);
        }

        mBufferedRTPPackets.clear();
      }

      //-----------------------------------------------------------------------
      void FakeListener::unregisterReceiver(UseReceiver &inReceiver)
      {
        ZS_LOG_BASIC(log("unregister receiver") + ZS_PARAM("receiver", inReceiver.getID()))

        AutoRecursiveLock lock(*this);

        auto receiver = mReceiver.lock();
        if (!receiver) {
          mReceiver.reset();
          return;
        }

        if (receiver->getID() != inReceiver.getID()) return;

        mReceiver.reset();
      }

      //-----------------------------------------------------------------------
      void FakeListener::getPackets(RTCPPacketList &outPacketList)
      {
        outPacketList.clear();

        AutoRecursiveLock lock(*this);
        for (auto iter = mBufferedRTCPPackets.begin(); iter != mBufferedRTCPPackets.end(); ++iter) {
          auto &rtcpPacket = (*iter).second;
          outPacketList.push_back(rtcpPacket);
        }
      }

      //-----------------------------------------------------------------------
      void FakeListener::notifyUnhandled(
                                         const String &muxID,
                                         const String &rid,
                                         IRTPTypes::SSRCType ssrc,
                                         IRTPTypes::PayloadType payloadType
                                         )
      {
        AutoRecursiveLock lock(*this);

        for (auto iter = mUnhandled.begin(); iter != mUnhandled.end(); ++iter)
        {
          auto &data = (*iter);

          if (data.mMuxID != muxID) continue;
          if (data.mRID != rid) continue;
          if (data.mSSRC != ssrc) continue;
          if (data.mPayloadType != payloadType) continue;

          ZS_LOG_TRACE(log("notified unhandled already processed") + ZS_PARAM("mux id", muxID) + ZS_PARAM("rid", rid) + ZS_PARAM("ssrc", ssrc) + ZS_PARAM("payload type", payloadType))
          return;
        }

        UnhandledData data;
        data.mMuxID = muxID;
        data.mRID = rid;
        data.mSSRC = ssrc;
        data.mPayloadType = payloadType;

        mUnhandled.push_back(data);

        ZS_LOG_BASIC(log("notified unhandled") + ZS_PARAM("mux id", muxID) + ZS_PARAM("rid", rid) + ZS_PARAM("ssrc", ssrc) + ZS_PARAM("payload type", payloadType))
        mSubscriptions.delegate()->onRTPListenerUnhandledRTP(mThisWeak.lock(), ssrc, payloadType, muxID, rid);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeListener => IRTPListenerForRTPSender
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeListener::registerSender(
                                        UseSenderPtr inSender,
                                        const Parameters &inParams,
                                        RTCPPacketList &outPacketList
                                        )
      {
        AutoRecursiveLock lock(*this);

        for (auto iter = mSenders.begin(); iter != mSenders.end(); ++iter) {
          auto sender = (*iter).lock();
          if (sender->getID() == inSender->getID()) return;
        }

        mSenders.push_back(inSender);

        for (auto iter = mBufferedRTCPPackets.begin(); iter != mBufferedRTCPPackets.end(); ++iter) {
          auto &rtcpPacket = (*iter).second;
          outPacketList.push_back(rtcpPacket);
        }
      }

      //-----------------------------------------------------------------------
      void FakeListener::unregisterSender(UseSender &inSender)
      {
        AutoRecursiveLock lock(*this);

        for (auto iter_doNotUse = mSenders.begin(); iter_doNotUse != mSenders.end(); ) {
          auto current = iter_doNotUse;
          ++iter_doNotUse;

          auto sender = (*current).lock();
          if (!sender) {
            mSenders.erase(current);
            continue;
          }
          if (sender->getID() != inSender.getID()) continue;
          mSenders.erase(current);
          return;
        }
      }
      

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeListener => IRTPListenerForSecureTransport
      #pragma mark

      //-----------------------------------------------------------------------
      bool FakeListener::handleRTPPacket(
                                         IICETypes::Components viaComponent,
                                         IICETypes::Components packetType,
                                         const BYTE *buffer,
                                         size_t bufferLengthInBytes
                                         )
      {
        RTPPacketPtr rtpPacket;
        RTCPPacketPtr rtcpPacket;

        UseReceiverPtr receiver;
        SenderList senders;

        ZS_LOG_BASIC(log("received RTC packet") + ZS_PARAM("via", IICETypes::toString(viaComponent)) + ZS_PARAM("packet type", IICETypes::toString(packetType)) + ZS_PARAM("buffer length", bufferLengthInBytes))

        {
          AutoRecursiveLock lock(*this);

          switch (packetType) {
            case IICETypes::Component_RTP: {
              rtpPacket = RTPPacket::create(buffer, bufferLengthInBytes);
              break;
            }
            case IICETypes::Component_RTCP: {
              rtcpPacket = RTCPPacket::create(buffer, bufferLengthInBytes);
              break;
            }
          }

          if ((!rtpPacket) &&
              (!rtcpPacket)) {
            ZS_LOG_WARNING(Basic, log("failed to parse packet"))
            return false;
          }

          if (rtcpPacket) {
            mBufferedRTCPPackets.push_back(TimeRTCPPacketPair(zsLib::now(), rtcpPacket));
          }

          receiver = mReceiver.lock();
          if ((!receiver) &&
              (rtpPacket)) {
            mBufferedRTPPackets.push_back(TimeRTPPacketPair(zsLib::now(), rtpPacket));
          }

          for (auto iter_doNotUse = mSenders.begin(); iter_doNotUse != mSenders.end(); ) {
            auto current = iter_doNotUse;
            ++iter_doNotUse;

            auto sender = (*current).lock();
            if (!sender) {
              mSenders.erase(current);
              continue;
            }

            senders.push_back(sender);
          }
        }

        if (rtcpPacket) {
          for (auto iter = senders.begin(); iter != senders.end(); ++iter) {
            auto &sender = (*iter);
            sender->handlePacket(viaComponent, rtcpPacket);
          }
          senders.clear();
        }

        if (!receiver) return true;

        if (rtpPacket) {
          return receiver->handlePacket(viaComponent, rtpPacket);
        }
        return receiver->handlePacket(viaComponent, rtcpPacket);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeListener => ITimerDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeListener::onTimer(TimerPtr timer)
      {
        if (timer == mCleanBuffersTimer) {

          auto tick = zsLib::now();

          while (mBufferedRTPPackets.size() > 0) {
            auto &bufferTime = mBufferedRTPPackets.front().first;

            if (bufferTime + Seconds(60) > tick) break;

            mBufferedRTPPackets.erase(mBufferedRTPPackets.begin());
          }

          while (mBufferedRTCPPackets.size() > 0) {
            auto &bufferTime = mBufferedRTCPPackets.front().first;

            if (bufferTime + Seconds(60) > tick) break;

            mBufferedRTCPPackets.erase(mBufferedRTCPPackets.begin());
          }
          return;
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeListener => IFakeListenerAsyncDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeListener::onForwardBufferedPacket(RTPPacketPtr packet)
      {
        UseReceiverPtr receiver;

        {
          AutoRecursiveLock lock(*this);
          receiver = mReceiver.lock();
        }

        if (!receiver) {
          ZS_LOG_WARNING(Basic, log("dropping packet as receiver is gone") + packet->toDebug())
          return;
        }

        ZS_LOG_DEBUG(log("forwarding packet to receiver") + ZS_PARAM("receiver", receiver->getID()) + packet->toDebug())

        receiver->handlePacket(IICETypes::Component_RTP, packet);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeListener => (friend RTPReceiverTester)
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeListener::setTransport(RTPReceiverTesterPtr tester)
      {
        AutoRecursiveLock lock(*this);

        mTester = tester;
      }

      //-----------------------------------------------------------------------
      IRTPListenerSubscriptionPtr FakeListener::subscribe(IRTPListenerDelegatePtr originalDelegate)
      {
        ZS_LOG_DETAIL(log("subscribing to listener"))

        AutoRecursiveLock lock(*this);
        if (!originalDelegate) return mDefaultSubscription;

        IRTPListenerSubscriptionPtr subscription = mSubscriptions.subscribe(originalDelegate, getAssociatedMessageQueue());

        IRTPListenerDelegatePtr delegate = mSubscriptions.delegate(subscription, true);

        if (delegate) {
          FakeListenerPtr pThis = mThisWeak.lock();

          for (auto iter = mUnhandled.begin(); iter != mUnhandled.end(); ++iter)
          {
            auto &data = (*iter);
            delegate->onRTPListenerUnhandledRTP(pThis, data.mSSRC, data.mPayloadType, data.mMuxID, data.mRID);
          }
        }

        return subscription;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeListener => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      Log::Params FakeListener::log(const char *message) const
      {
        ElementPtr objectEl = Element::create("ortc::test::rtpreceiver::FakeListener");
        UseServicesHelper::debugAppend(objectEl, "id", FakeListener::getID());
        return Log::Params(message, objectEl);
      }

      //-----------------------------------------------------------------------
      void FakeListener::cancel()
      {
        if (mDefaultSubscription) {
          mDefaultSubscription->cancel();
          mDefaultSubscription.reset();
        }

        mReceiver.reset();

        mBufferedRTPPackets.clear();
        mBufferedRTCPPackets.clear();

        mUnhandled.clear();

        mSenders.clear();
      }


      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeReceiverChannel
      #pragma mark

      //-----------------------------------------------------------------------
      FakeReceiverChannel::FakeReceiverChannel(IMessageQueuePtr queue) :
        RTPReceiverChannel(Noop(true), queue)
      {
      }

      //-----------------------------------------------------------------------
      FakeReceiverChannel::~FakeReceiverChannel()
      {
        mThisWeak.reset();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeReceiverChannel => IRTPReceiverChannelForRTPReceiver
      #pragma mark

      //-----------------------------------------------------------------------
      ElementPtr FakeReceiverChannel::toDebug() const
      {
        AutoRecursiveLock lock(*this);

        ElementPtr result = Element::create("ortc::test::rtpreceiver::FakeReceiverChannel");

        UseServicesHelper::debugAppend(result, "tester", (bool)mTester.lock());

        UseServicesHelper::debugAppend(result, "parameters", mParameters ? mParameters->toDebug() : ElementPtr());

        UseServicesHelper::debugAppend(result, "expect buffers", mExpectBuffers.size());
        UseServicesHelper::debugAppend(result, "expect parameters", mExpectParameters.size());
        UseServicesHelper::debugAppend(result, "expect states", mExpectStates.size());

        auto receiver = mReceiver.lock();
        UseServicesHelper::debugAppend(result, "receiver", receiver ? receiver->getID() : 0);

        return result;
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::create(
                                       RTPReceiverPtr inReceiver,
                                       const Parameters &params,
                                       const RTCPPacketList &packets
                                       )
      {
        UseReceiverPtr receiver(inReceiver);

        ZS_LOG_BASIC(log("create called") + ZS_PARAM("receiver", receiver->getID()) + params.toDebug())

        {
          AutoRecursiveLock lock(*this);

          mReceiver = receiver;

          TESTING_EQUAL(params.hash(), mParameters->hash())
        }

        notifyPackets(make_shared<RTCPPacketList>(packets));
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::notifyTransportState(ISecureTransport::States state)
      {
        IFakeReceiverChannelAsyncDelegateProxy::create(mThisWeak.lock())->onState(state);
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::notifyPacket(RTPPacketPtr packet)
      {
        IFakeReceiverChannelAsyncDelegateProxy::create(mThisWeak.lock())->onRTPPacket(packet);
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::notifyPackets(RTCPPacketListPtr packets)
      {
        ZS_LOG_BASIC(log("notified packets") + ZS_PARAM("packets", packets->size()))

        for (auto iter = packets->begin(); iter != packets->end(); ++iter) {
          handlePacket(*iter);
        }
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::notifyUpdate(const Parameters &params)
      {
        IFakeReceiverChannelAsyncDelegateProxy::create(mThisWeak.lock())->onUpdate(make_shared<Parameters>(params));
      }

      //-----------------------------------------------------------------------
      bool FakeReceiverChannel::handlePacket(RTPPacketPtr packet)
      {
        ZS_LOG_BASIC(log("handle packet") + packet->toDebug())

        RTPReceiverTesterPtr tester;

        {
          AutoRecursiveLock lock(*this);

          tester = mTester.lock();
          TESTING_CHECK(tester)

          TESTING_CHECK(mExpectBuffers.size() > 0)

          TESTING_CHECK(0 == UseServicesHelper::compare(*(mExpectBuffers.front()), *(packet->mBuffer)))

          mExpectStates.pop_front();
        }
        
        tester->notifyReceivedPacket();

        return true;
      }

      //-----------------------------------------------------------------------
      bool FakeReceiverChannel::handlePacket(RTCPPacketPtr packet)
      {
        ZS_LOG_BASIC(log("handle packet") + packet->toDebug())

        RTPReceiverTesterPtr tester;

        {
          AutoRecursiveLock lock(*this);

          tester = mTester.lock();
          TESTING_CHECK(tester)

          TESTING_CHECK(mExpectBuffers.size() > 0)

          TESTING_CHECK(0 == UseServicesHelper::compare(*(mExpectBuffers.front()), *(packet->mBuffer)))

          mExpectStates.pop_front();
        }

        tester->notifyReceivedPacket();

        return true;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeReceiverChannel => IFakeReceiverChannelAsyncDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::onState(ISecureTransportTypes::States state)
      {
        ZS_LOG_BASIC(log("on state") + ZS_PARAM("state", ISecureTransportTypes::toString(state)))

        RTPReceiverTesterPtr tester;

        {
          AutoRecursiveLock lock(*this);

          tester = mTester.lock();
          TESTING_CHECK(tester)

          TESTING_CHECK(mExpectStates.size() > 0)

          TESTING_EQUAL(state, mExpectStates.front())

          mExpectStates.pop_front();
        }

        tester->notifyReceiverChannelOfSecureTransportState();
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::onRTPPacket(RTPPacketPtr packet)
      {
        handlePacket(packet);
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::onRTCPPacket(RTCPPacketPtr packet)
      {
        handlePacket(packet);
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::onUpdate(ParametersPtr params)
      {
        ZS_LOG_BASIC(log("on updatae") + params->toDebug())

        RTPReceiverTesterPtr tester;

        {
          AutoRecursiveLock lock(*this);

          tester = mTester.lock();
          TESTING_CHECK(tester)

          TESTING_CHECK(mExpectParameters.size() > 0)

          TESTING_EQUAL(params->hash(), mExpectParameters.front()->hash())

          mExpectParameters.pop_front();
        }
        
        tester->notifyReceivedChannelUpdate();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeReceiverChannel => (friend RTPReceiverTester)
      #pragma mark

      //-----------------------------------------------------------------------
      FakeReceiverChannelPtr FakeReceiverChannel::create(
                                                         IMessageQueuePtr queue,
                                                         const Parameters &expectedParams
                                                         )
      {
        FakeReceiverChannelPtr pThis(make_shared<FakeReceiverChannel>(queue));
        pThis->mThisWeak = pThis;
        pThis->mParameters = make_shared<Parameters>(expectedParams);
        return pThis;
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::setTransport(RTPReceiverTesterPtr tester)
      {
        AutoRecursiveLock lock(*this);
        mTester = tester;
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::sendPacket(RTCPPacketPtr packet)
      {
        UseReceiverPtr receiver;

        {
          AutoRecursiveLock lock(*this);
          receiver = mReceiver.lock();

          TESTING_CHECK(receiver)
        }

        receiver->sendPacket(packet);
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::expectState(ISecureTransport::States state)
      {
        AutoRecursiveLock lock(*this);
        mExpectStates.push_back(state);
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::expectUpdate(const Parameters &params)
      {
        AutoRecursiveLock lock(*this);
        mExpectParameters.push_back(make_shared<Parameters>(params));
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::expectData(SecureByteBlockPtr data)
      {
        AutoRecursiveLock lock(*this);
        mExpectBuffers.push_back(data);
      }

      //-----------------------------------------------------------------------
      void FakeReceiverChannel::stop()
      {
        AutoRecursiveLock lock(*this);

        mReceiver.reset();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeReceiverChannel => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      Log::Params FakeReceiverChannel::log(const char *message) const
      {
        ElementPtr objectEl = Element::create("ortc::test::rtpreceiver::FakeReceiverChannel");
        UseServicesHelper::debugAppend(objectEl, "id", RTPReceiverChannel::getID());
        return Log::Params(message, objectEl);
      }


      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeMediaStreamTrack
      #pragma mark

      //-----------------------------------------------------------------------
      FakeMediaStreamTrack::FakeMediaStreamTrack(IMessageQueuePtr queue) :
        MediaStreamTrack(Noop(true), queue)
      {
      }

      //-----------------------------------------------------------------------
      FakeMediaStreamTrack::~FakeMediaStreamTrack()
      {
        mThisWeak.reset();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeMediaStreamTrack => IMediaStreamTrackForRTPReceiver
      #pragma mark

      //-----------------------------------------------------------------------
      ElementPtr FakeMediaStreamTrack::toDebug() const
      {
        AutoRecursiveLock lock(*this);

        ElementPtr result = Element::create("ortc::test::rtpreceiver::FakeMediaStreamTrack");

        UseServicesHelper::debugAppend(result, "kind", IMediaStreamTrackTypes::toString(mKind));

        UseServicesHelper::debugAppend(result, "tester", (bool)mTester.lock());

        auto receiver = mReceiver.lock();
        UseServicesHelper::debugAppend(result, "receiver", receiver ? receiver->getID() : 0);

        UseServicesHelper::debugAppend(result, "expect active channel IDs", mExpectActiveChannelIDs.size());

        return result;
      }

      //-----------------------------------------------------------------------
      void FakeMediaStreamTrack::create(IMediaStreamTrackTypes::Kinds kind)
      {
        ZS_LOG_BASIC(log("create called") + ZS_PARAM("kind", IMediaStreamTrackTypes::toString(kind)))

        AutoRecursiveLock lock(*this);
        TESTING_CHECK(mKind == kind)
      }

      //-----------------------------------------------------------------------
      void FakeMediaStreamTrack::notifyActiveReceiverChannel(RTPReceiverChannelPtr inChannel)
      {
        UseReceiverChannelPtr channel(inChannel);

        ZS_LOG_BASIC(log("notify active receiver channel") + ZS_PARAM("channel", channel->getID()))

        TESTING_CHECK(channel)

        RTPReceiverTesterPtr tester;

        {
          AutoRecursiveLock lock(*this);

          tester = mTester.lock();
          TESTING_CHECK(tester)

          TESTING_CHECK(mExpectActiveChannelIDs.size() > 0)

          auto channelID = channel->getID();

          TESTING_EQUAL(channelID, mExpectActiveChannelIDs.front())

          mExpectActiveChannelIDs.pop_front();
        }

        tester->notifyActiveReceiverChannel();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeMediaStreamTrack => (friend RTPReceiverTester)
      #pragma mark

      //-----------------------------------------------------------------------
      FakeMediaStreamTrackPtr FakeMediaStreamTrack::create(
                                                           IMessageQueuePtr queue,
                                                           IMediaStreamTrackTypes::Kinds kind
                                                           )
      {
        FakeMediaStreamTrackPtr pThis(make_shared<FakeMediaStreamTrack>(queue));
        pThis->mThisWeak = pThis;
        return pThis;
      }

      //-----------------------------------------------------------------------
      void FakeMediaStreamTrack::setTransport(RTPReceiverTesterPtr tester)
      {
        AutoRecursiveLock lock(*this);
        mTester = tester;
      }

      //-----------------------------------------------------------------------
      void FakeMediaStreamTrack::expectActiveChannel(ReceiverChannelID channelID)
      {
        ZS_LOG_BASIC(log("expect active channel id") + ZS_PARAM("channel id", channelID))

        AutoRecursiveLock lock(*this);
        mExpectActiveChannelIDs.push_back(channelID);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeMediaStreamTrack => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      Log::Params FakeMediaStreamTrack::log(const char *message) const
      {
        ElementPtr objectEl = Element::create("ortc::test::rtpreceiver::FakeMediaStreamTrack");
        UseServicesHelper::debugAppend(objectEl, "id", MediaStreamTrack::getID());
        return Log::Params(message, objectEl);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSender
      #pragma mark

      //-----------------------------------------------------------------------
      FakeSender::FakeSender() :
        RTPSender(Noop(true))
      {
      }

      //-----------------------------------------------------------------------
      FakeSender::~FakeSender()
      {
        mThisWeak.reset();
      }

      //-----------------------------------------------------------------------
      FakeSenderPtr FakeSender::create()
      {
        FakeSenderPtr pThis(make_shared<FakeSender>());
        pThis->mThisWeak = pThis;
        return pThis;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSender => IRTPSenderForRTPReceiver
      #pragma mark

      //-----------------------------------------------------------------------
      ElementPtr FakeSender::toDebug() const
      {
        AutoRecursiveLock lock(*this);

        ElementPtr result = Element::create("ortc::test::rtpreceiver::FakeSender");

        UseServicesHelper::debugAppend(result, "tester", (bool)mTester.lock());
        UseServicesHelper::debugAppend(result, mParameters ? mParameters->toDebug() : ElementPtr());
        UseServicesHelper::debugAppend(result, "expect buffers", mExpectBuffers.size());
        UseServicesHelper::debugAppend(result, "listener", mListener ? mListener->getID() : 0);
        UseServicesHelper::debugAppend(result, "secure transport", mSecureTransport ? mSecureTransport->getID() : 0);

        return result;
      }

      //-----------------------------------------------------------------------
      bool FakeSender::handlePacket(
                                    IICETypes::Components viaTransport,
                                    RTCPPacketPtr packet
                                    )
      {
        ZS_LOG_BASIC(log("received RTCP packet") + packet->toDebug())

        AutoRecursiveLock lock(*this);

        TESTING_CHECK(mExpectBuffers.size() > 0)

        TESTING_CHECK(0 == UseServicesHelper::compare(*(mExpectBuffers.front()), *(packet->mBuffer)))

        mExpectBuffers.pop_front();

        auto tester = mTester.lock();

        if (tester) tester->notifyReceivedPacket();

        return true;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSender => (friend RTPReceiverTester)
      #pragma mark

      //-----------------------------------------------------------------------
      void FakeSender::setTransport(RTPReceiverTesterPtr tester)
      {
        auto secureTransport = (((bool)tester) ? tester->getFakeSecureTransport() : FakeSecureTransportPtr());
        TESTING_CHECK(secureTransport)


        AutoRecursiveLock lock(*this);

        mTester = tester;

        if (mListener) {
          ZS_LOG_BASIC(log("unregistering receiver") + ZS_PARAM("listener", mListener->getID()))
          mListener->unregisterSender(*this);
        }

        if (secureTransport) {
          mListener = secureTransport->getListener();

          if (mParameters) {
            ZS_LOG_BASIC(log("registering receiver") + ZS_PARAM("listener", mListener->getID()) + mParameters->toDebug())

            RTCPPacketList packetList;
            mListener->registerSender(mThisWeak.lock(), *mParameters, packetList);

            for (auto iter = packetList.begin(); iter != packetList.end(); ++iter) {
              // fake this as if it was a received packet
              handlePacket(IICETypes::Component_RTP, *iter);
            }
          }
          
        } else {
          mListener.reset();
        }
      }

      //-----------------------------------------------------------------------
      void FakeSender::send(const Parameters &parameters)
      {
        AutoRecursiveLock lock(*this);

        mParameters = make_shared<Parameters>(parameters);

        if (mListener) {
          RTCPPacketList packetList;
          mListener->registerSender(mThisWeak.lock(), *mParameters, packetList);

          for (auto iter = packetList.begin(); iter != packetList.end(); ++iter) {
            // fake this as if it was a received packet
            handlePacket(IICETypes::Component_RTP, *iter);
          }
        }
      }

      //-----------------------------------------------------------------------
      void FakeSender::stop()
      {
        AutoRecursiveLock lock(*this);

        if (mListener) {
          mListener->unregisterSender(*this);
        }
      }

      //-----------------------------------------------------------------------
      void FakeSender::expectData(SecureByteBlockPtr data)
      {
        TESTING_CHECK((bool)data)

        AutoRecursiveLock lock(*this);

        ZS_LOG_TRACE(log("expecting buffer") + ZS_PARAM("buffer size", data->SizeInBytes()))

        mExpectBuffers.push_back(data);
      }

      //-----------------------------------------------------------------------
      void FakeSender::sendPacket(SecureByteBlockPtr buffer)
      {
        UseSecureTransportPtr transport;

        {
          AutoRecursiveLock lock(*this);

          auto tester = mTester.lock();
          TESTING_CHECK(tester)

          transport = tester->getFakeSecureTransport();
          TESTING_CHECK(transport)
        }

        transport->sendPacket(IICETypes::Component_RTP, IICETypes::Component_RTP, buffer->BytePtr(), buffer->SizeInBytes());
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark FakeSender => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      Log::Params FakeSender::log(const char *message) const
      {
        ElementPtr objectEl = Element::create("ortc::test::rtpreceiver::FakeSender");
        UseServicesHelper::debugAppend(objectEl, "id", FakeSender::getID());
        return Log::Params(message, objectEl);
      }


      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester::UnhandledEventData
      #pragma mark

      //-----------------------------------------------------------------------
      RTPReceiverTester::UnhandledEventData::UnhandledEventData(
                                                                DWORD ssrc,
                                                                BYTE pt,
                                                                const char *mid,
                                                                const char *rid
                                                                ) :
        mSSRC(ssrc),
        mPT(pt),
        mMID(mid),
        mRID(rid)
      {
      }

      //-----------------------------------------------------------------------
      bool RTPReceiverTester::UnhandledEventData::operator==(const UnhandledEventData &op2) const
      {
        return ((mSSRC == op2.mSSRC) &&
                (mPT == op2.mPT) &&
                (mMID == op2.mMID) &&
                (mRID == op2.mRID));
      }
      
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester::Expectations
      #pragma mark

      //-----------------------------------------------------------------------
      bool RTPReceiverTester::Expectations::operator==(const Expectations &op2) const
      {
        return (mUnhandled == op2.mUnhandled) &&
               (mReceivedPackets == op2.mReceivedPackets) &&

               (mError == op2.mError) &&
               (mChannelUpdate == op2.mChannelUpdate) &&

               (mActiveReceiverChannel == op2.mActiveReceiverChannel) &&
               (mReceiverChannelOfSecureTransportState == op2.mReceiverChannelOfSecureTransportState) &&

               (mKind == op2.mKind);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester::OverrideReceiverChannelFactory
      #pragma mark

      //-----------------------------------------------------------------------
      RTPReceiverTester::OverrideReceiverChannelFactoryPtr RTPReceiverTester::OverrideReceiverChannelFactory::create(RTPReceiverTesterPtr tester)
      {
        OverrideReceiverChannelFactoryPtr pThis(make_shared<OverrideReceiverChannelFactory>());
        pThis->mTester = tester;
        return pThis;
      }

      //-----------------------------------------------------------------------
      RTPReceiverChannelPtr RTPReceiverTester::OverrideReceiverChannelFactory::create(
                                                                                      RTPReceiverPtr receiver,
                                                                                      const Parameters &params,
                                                                                      const RTCPPacketList &packets
                                                                                      )
      {
        auto tester = mTester.lock();
        TESTING_CHECK(tester)

        return tester->create(receiver, params, packets);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester::OverrideReceiverChannelFactory
      #pragma mark

      //-----------------------------------------------------------------------
      RTPReceiverTester::OverrideMediaStreamTrackFactoryPtr RTPReceiverTester::OverrideMediaStreamTrackFactory::create(RTPReceiverTesterPtr tester)
      {
        OverrideMediaStreamTrackFactoryPtr pThis(make_shared<OverrideMediaStreamTrackFactory>());
        pThis->mTester = tester;
        return pThis;
      }

      //-----------------------------------------------------------------------
      MediaStreamTrackPtr RTPReceiverTester::OverrideMediaStreamTrackFactory::create(
                                                                                     IMediaStreamTrackTypes::Kinds kind,
                                                                                     bool remote,
                                                                                     TrackConstraintsPtr constraints
                                                                                     )
      {
        TESTING_CHECK(false)
        return MediaStreamTrackPtr();
      }

      //-----------------------------------------------------------------------
      MediaStreamTrackPtr RTPReceiverTester::OverrideMediaStreamTrackFactory::create(IMediaStreamTrackTypes::Kinds kind)
      {
        auto tester = mTester.lock();
        TESTING_CHECK(tester)

        return tester->create(kind);
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester
      #pragma mark

      //-----------------------------------------------------------------------
      RTPReceiverTesterPtr RTPReceiverTester::create(
                                                     IMessageQueuePtr queue,
                                                     Milliseconds packetDelay
                                                     )
      {
        RTPReceiverTesterPtr pThis(new RTPReceiverTester(queue));
        pThis->mThisWeak = pThis;
        pThis->init(packetDelay);
        return pThis;
      }

      //-----------------------------------------------------------------------
      RTPReceiverTester::RTPReceiverTester(IMessageQueuePtr queue) :
        SharedRecursiveLock(SharedRecursiveLock::create()),
        MessageQueueAssociator(queue)
      {
        ZS_LOG_BASIC(log("rtpreceiver tester"))
      }

      //-----------------------------------------------------------------------
      RTPReceiverTester::~RTPReceiverTester()
      {
        ZS_LOG_BASIC(log("rtpreceiver tester"))

        RTPReceiverChannelFactory::override(RTPReceiverChannelFactoryPtr());
        MediaStreamTrackFactory::override(MediaStreamTrackFactoryPtr());
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::init(Milliseconds packetDelay)
      {
        AutoRecursiveLock lock(*this);

        RTPReceiverChannelFactory::override(OverrideReceiverChannelFactory::create(mThisWeak.lock()));
        MediaStreamTrackFactory::override(OverrideMediaStreamTrackFactory::create(mThisWeak.lock()));

        mICETransport = FakeICETransport::create(getAssociatedMessageQueue(), packetDelay);
        mDTLSTransport = FakeSecureTransport::create(getAssociatedMessageQueue(), mICETransport);

        auto listener = mDTLSTransport->getListener();
        TESTING_CHECK(listener)

        mListenerSubscription = FakeListener::convert(listener)->subscribe(mThisWeak.lock());

        mReceiver = IRTPReceiver::create(mThisWeak.lock(), mDTLSTransport);
      }

      //-----------------------------------------------------------------------
      bool RTPReceiverTester::matches(const Expectations &op2)
      {
        AutoRecursiveLock lock(*this);
        return mExpectations == op2;
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::close()
      {
        AutoRecursiveLock lock(*this);

        mReceiver->stop();

        for (auto iter = mAttached.begin(); iter != mAttached.end(); ++iter) {
          auto receiverChannel = (*iter).second.first.lock();
          auto &sender = (*iter).second.second;

          if (receiverChannel) {
            receiverChannel->stop();
          }
          if (sender) {
            sender->stop();
          }
        }

        mAttached.clear();
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::closeByReset()
      {
        AutoRecursiveLock lock(*this);
        mICETransport.reset();
        mDTLSTransport.reset();

        mReceiver.reset();
      }

      //-----------------------------------------------------------------------
      RTPReceiverTester::Expectations RTPReceiverTester::getExpectations() const
      {
        return mExpectations;
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::state(IICETransport::States newState)
      {
        FakeICETransportPtr transport;
        {
          AutoRecursiveLock lock(*this);
          transport = mICETransport;
        }
        transport->state(newState);
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::state(IDTLSTransport::States newState)
      {
        FakeSecureTransportPtr transport;
        {
          AutoRecursiveLock lock(*this);
          transport = mDTLSTransport;
        }
        transport->state(newState);
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::setClientRole(bool clientRole)
      {
        FakeSecureTransportPtr transport;
        {
          AutoRecursiveLock lock(*this);
          transport = mDTLSTransport;
        }
        transport->setClientRole(clientRole);
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::connect(RTPReceiverTesterPtr remote)
      {
        AutoRecursiveLock lock(*this);
        AutoRecursiveLock lock2(*remote);

        TESTING_CHECK(remote)

        auto localTransport = getICETransport();
        auto remoteTransport = remote->getICETransport();

        TESTING_CHECK((bool)localTransport)
        TESTING_CHECK((bool)remoteTransport)

        localTransport->linkTransport(remoteTransport);
        remoteTransport->linkTransport(localTransport);

        mConnectedTester = remote;
        remote->mConnectedTester = mThisWeak.lock();
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::createReceiverChannel(
                                                    const char *receiverID,
                                                    const char *parametersID
                                                    )
      {
        FakeReceiverChannelPtr receiverChannel = getReceiverChannel(receiverID);

        if (!receiverChannel) {
          auto params = getParameters(parametersID);
          TESTING_CHECK(params)
          receiverChannel = FakeReceiverChannel::create(getAssociatedMessageQueue(), *params);
          attach(receiverID, receiverChannel);
        }

        TESTING_CHECK(receiverChannel)
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::createSender(const char *senderID)
      {
        FakeSenderPtr sender = getSender(senderID);

        if (!sender) {
          sender = FakeSender::create();
          attach(senderID, sender);
        }

        TESTING_CHECK(sender)
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::send(
                                   const char *senderID,
                                   const char *parametersID
                                   )
      {
        FakeSenderPtr sender = getSender(senderID);

        if (!sender) {
          sender = FakeSender::create();
          attach(senderID, sender);
        }

        TESTING_CHECK(sender)

        auto params = getParameters(parametersID);
        TESTING_CHECK(params)

        sender->send(*params);
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::receive(const char *parametersID)
      {
        TESTING_CHECK(mReceiver)

        auto params = getParameters(parametersID);
        TESTING_CHECK(params)

        mReceiver->receive(*params);
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::stop(const char *senderOrReceiverID)
      {
        AutoRecursiveLock lock(*this);

        auto found = mAttached.find(String(senderOrReceiverID));
        TESTING_CHECK(found != mAttached.end())

        auto receiverChannel = (*found).second.first.lock();
        auto sender = (*found).second.second;

        if (receiverChannel) {
          receiverChannel->stop();
        }
        if (sender) {
          sender->stop();
        }

        mAttached.erase(found);
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::attach(
                                     const char *receiverChannelID,
                                     FakeReceiverChannelPtr receiverChannel
                                     )
      {
        TESTING_CHECK(receiverChannel)

        String receiverChannelIDStr(receiverChannelID);

        AutoRecursiveLock lock(*this);

        for (auto iter_doNotUse = mFakeReceiverChannelCreationList.begin(); iter_doNotUse != mFakeReceiverChannelCreationList.end(); )
        {
          auto current = iter_doNotUse;
          ++iter_doNotUse;

          auto &channelID = (*current).first;

          if (channelID != receiverChannelIDStr) continue;

          mFakeReceiverChannelCreationList.erase(current);
        }

        mFakeReceiverChannelCreationList.push_back(FakeReceiverChannelPair(receiverChannelIDStr, receiverChannel));

        auto found = mAttached.find(receiverChannelIDStr);

        if (found != mAttached.end()) {
          auto &previousReceiverWeak = (*found).second.first;
          auto previousReceiver = previousReceiverWeak.lock();
          if (previousReceiver) {
            previousReceiver->stop();
          }

          previousReceiver = receiverChannel;
          receiverChannel->setTransport(mThisWeak.lock());
          return;
        }

        mAttached[receiverChannelIDStr] = FakeReceiverChannelFakeSenderPair(receiverChannel, FakeSenderPtr());
        receiverChannel->setTransport(mThisWeak.lock());
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::attach(
                                     const char *senderID,
                                     FakeSenderPtr sender
                                     )
      {
        AutoRecursiveLock lock(*this);

        auto found = mAttached.find(String(senderID));

        if (found != mAttached.end()) {
          auto &previousSender = (*found).second.second;
          if (previousSender) {
            previousSender->stop();
          }

          previousSender = sender;
          sender->setTransport(mThisWeak.lock());
          return;
        }

        mAttached[String(senderID)] = FakeReceiverChannelFakeSenderPair(FakeReceiverChannelPtr(), sender);
        sender->setTransport(mThisWeak.lock());
      }

      //-----------------------------------------------------------------------
      FakeReceiverChannelPtr RTPReceiverTester::detachReceiverChannel(const char *receiverChannelID)
      {
        String receiverChannelIDStr(receiverChannelID);

        AutoRecursiveLock lock(*this);

        auto found = mAttached.find(receiverChannelIDStr);
        TESTING_CHECK(found != mAttached.end())

        auto &currentReceiverWeak = (*found).second.first;
        auto &currentSender = (*found).second.second;

        FakeReceiverChannelPtr receiverChannel = currentReceiverWeak.lock();

        if (receiverChannel) {
          receiverChannel->setTransport(RTPReceiverTesterPtr());
        }

        currentReceiverWeak.reset();

        if (!currentSender) mAttached.erase(found);

        if (receiverChannel) {
          for (auto iter_doNotUse = mFakeReceiverChannelCreationList.begin(); iter_doNotUse != mFakeReceiverChannelCreationList.end(); )
          {
            auto current = iter_doNotUse;
            ++iter_doNotUse;

            auto &channelID = (*current).first;

            if (channelID != receiverChannelIDStr) continue;

            mFakeReceiverChannelCreationList.erase(current);
          }
        }

        return receiverChannel;
      }
      
      //-----------------------------------------------------------------------
      FakeSenderPtr RTPReceiverTester::detachSender(const char *senderID)
      {
        AutoRecursiveLock lock(*this);

        auto found = mAttached.find(String(senderID));
        TESTING_CHECK(found != mAttached.end())

        auto &currentReceiver = (*found).second.first;
        auto &currentSender = (*found).second.second;

        FakeSenderPtr sender = currentSender;

        if (sender) {
          sender->setTransport(RTPReceiverTesterPtr());
        }

        currentSender.reset();
        if (!currentReceiver.lock()) mAttached.erase(found);
        
        return sender;
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::expectingUnhandled(
                                                 SSRCType ssrc,
                                                 PayloadType payloadType,
                                                 const char *mid,
                                                 const char *rid
                                                 )
      {
        AutoRecursiveLock lock(*this);

        mExpectingUnhandled.push_back(UnhandledEventData(ssrc, payloadType, mid, rid));
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::expectReceiveChannelUpdate(
                                                         const char *receiverChannelID,
                                                         const Parameters &params
                                                         )
      {
        FakeReceiverChannelPtr receiverChannel = getReceiverChannel(receiverChannelID);

        TESTING_CHECK(receiverChannel)

        receiverChannel->expectUpdate(params);
      }
      
      //-----------------------------------------------------------------------
      void RTPReceiverTester::expectState(
                                          const char *receiverChannelID,
                                          ISecureTransportTypes::States state
                                          )
      {
        FakeReceiverChannelPtr receiverChannel = getReceiverChannel(receiverChannelID);

        TESTING_CHECK(receiverChannel)

        receiverChannel->expectState(state);
      }
      
      //-----------------------------------------------------------------------
      void RTPReceiverTester::expectActiveChannel(const char *receiverChannelID)
      {
        FakeMediaStreamTrackPtr track;
        FakeReceiverChannelPtr receiverChannel;

        {
          AutoRecursiveLock lock(*this);

          TESTING_CHECK(mMediaStreamTrack)

          receiverChannel = getReceiverChannel(receiverChannelID);

          TESTING_CHECK(receiverChannel)

          track = mMediaStreamTrack;
        }

        track->expectActiveChannel(receiverChannel->getID());
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::store(
                                    const char *packetID,
                                    RTPPacketPtr packet
                                    )
      {
        TESTING_CHECK(packet)
        AutoRecursiveLock lock(*this);

        auto found = mPackets.find(String(packetID));
        if (found != mPackets.end()) {
          (*found).second.first = packet;
          return;
        }

        mPackets[String(packetID)] = PacketPair(packet, RTCPPacketPtr());
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::store(
                                    const char *packetID,
                                    RTCPPacketPtr packet
                                    )
      {
        TESTING_CHECK(packet)
        AutoRecursiveLock lock(*this);

        auto found = mPackets.find(String(packetID));
        if (found != mPackets.end()) {
          (*found).second.second = packet;
          return;
        }

        mPackets[String(packetID)] = PacketPair(RTPPacketPtr(), packet);
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::store(
                                    const char *parametersID,
                                    const Parameters &params
                                    )
      {
        AutoRecursiveLock lock(*this);

        auto found = mParameters.find(String(parametersID));
        if (found != mParameters.end()) {
          (*found).second = make_shared<Parameters>(params);
          return;
        }

        mParameters[String(parametersID)] = make_shared<Parameters>(params);
      }

      //-----------------------------------------------------------------------
      RTPPacketPtr RTPReceiverTester::getRTPPacket(const char *packetID)
      {
        AutoRecursiveLock lock(*this);

        auto found = mPackets.find(String(packetID));
        if (found == mPackets.end()) return RTPPacketPtr();
        return (*found).second.first;
      }

      //-----------------------------------------------------------------------
      RTCPPacketPtr RTPReceiverTester::getRTCPPacket(const char *packetID)
      {
        AutoRecursiveLock lock(*this);

        auto found = mPackets.find(String(packetID));
        if (found == mPackets.end()) return RTCPPacketPtr();
        return (*found).second.second;
      }

      //-----------------------------------------------------------------------
      RTPReceiverTester::ParametersPtr RTPReceiverTester::getParameters(const char *parametersID)
      {
        AutoRecursiveLock lock(*this);

        auto found = mParameters.find(String(parametersID));
        if (found == mParameters.end()) return ParametersPtr();
        return make_shared<Parameters>(*((*found).second));
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::sendPacket(
                                         const char *packetID,
                                         const char *senderOrReceiverChannelID
                                         )
      {
        RTPPacketPtr rtp;
        RTCPPacketPtr rtcp;

        {
          AutoRecursiveLock lock(*this);
          auto found = mPackets.find(String(packetID));
          TESTING_CHECK(found != mPackets.end())

          rtp = (*found).second.first;
          rtcp = (*found).second.second;
        }

        if (rtp) {
          sendData(senderOrReceiverChannelID, rtp->buffer());
        }
        if (rtcp) {
          {
            FakeReceiverChannelPtr receiverChannel = getReceiverChannel(senderOrReceiverChannelID);
            if (receiverChannel) {
              receiverChannel->sendPacket(rtcp);
              return;
            }
          }

          sendData(senderOrReceiverChannelID, rtcp->buffer());
        }
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::expectPacket(
                                           const char *packetID,
                                           const char *senderOrReceiverID
                                           )
      {
        RTPPacketPtr rtp;
        RTCPPacketPtr rtcp;

        {
          AutoRecursiveLock lock(*this);
          auto found = mPackets.find(String(packetID));
          TESTING_CHECK(found != mPackets.end())

          rtp = (*found).second.first;
          rtcp = (*found).second.second;
        }

        if (rtp) {
          expectData(senderOrReceiverID, rtp->buffer());
        }
        if (rtcp) {
          expectData(senderOrReceiverID, rtcp->buffer());
        }
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester::IRTPReceiverDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void RTPReceiverTester::onRTPListenerUnhandledRTP(
                                                        IRTPListenerPtr receiver,
                                                        SSRCType ssrc,
                                                        PayloadType payloadType,
                                                        const char *mid,
                                                        const char *rid
                                                        )
      {
        ZS_LOG_BASIC(log("rtp listener unhandled rtp event") + ZS_PARAM("receiver id", receiver->getID()) + ZS_PARAM("ssrc", ssrc) + ZS_PARAM("payload type", payloadType) + ZS_PARAM("mid", mid) + ZS_PARAM("rid", rid))

        AutoRecursiveLock lock(*this);
        ++mExpectations.mUnhandled;

        TESTING_CHECK(mExpectingUnhandled.size() > 0)

        auto &expecting = mExpectingUnhandled.front();

        TESTING_CHECK(expecting == UnhandledEventData(ssrc, payloadType, mid, rid))

        mExpectingUnhandled.pop_front();
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester::IRTPReceiverDelegate
      #pragma mark

      //-----------------------------------------------------------------------
      void RTPReceiverTester::onRTPReceiverError(
                                                 IRTPReceiverPtr receiver,
                                                 ErrorCode errorCode,
                                                 String errorReason
                                                 )
      {
        ZS_LOG_BASIC(log("rtp receiver error") + ZS_PARAM("receiver", receiver->getID()) + ZS_PARAM("error code", errorCode) + ZS_PARAM("error reason", errorReason))

        AutoRecursiveLock lock(*this);
        ++mExpectations.mError;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester => (friend fake sender/receiver)
      #pragma mark

      //-----------------------------------------------------------------------
      FakeSecureTransportPtr RTPReceiverTester::getFakeSecureTransport() const
      {
        return mDTLSTransport;
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::notifyReceivedPacket()
      {
        ZS_LOG_BASIC(log("notified received packet"))

        AutoRecursiveLock lock(*this);
        ++mExpectations.mReceivedPackets;
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::notifyReceivedChannelUpdate()
      {
        ZS_LOG_BASIC(log("notified received channel update"))

        AutoRecursiveLock lock(*this);
        ++mExpectations.mChannelUpdate;
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::notifyActiveReceiverChannel()
      {
        ZS_LOG_BASIC(log("notified active channel"))

        AutoRecursiveLock lock(*this);
        ++mExpectations.mActiveReceiverChannel;
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::notifyReceiverChannelOfSecureTransportState()
      {
        ZS_LOG_BASIC(log("notified receiver channel of secure transport state"))

        AutoRecursiveLock lock(*this);
        ++mExpectations.mReceiverChannelOfSecureTransportState;
      }

      //-----------------------------------------------------------------------
      RTPReceiverChannelPtr RTPReceiverTester::create(
                                                      RTPReceiverPtr receiver,
                                                      const Parameters &params,
                                                      const RTCPPacketList &packets
                                                      )
      {
        AutoRecursiveLock lock(*this);

        TESTING_CHECK(mFakeReceiverChannelCreationList.size() > 0)

        FakeReceiverChannelPtr receiverChannel = mFakeReceiverChannelCreationList.front().second;

        TESTING_CHECK(receiverChannel)

        receiverChannel->create(receiver, params, packets);

        return receiverChannel;
      }

      //-----------------------------------------------------------------------
      MediaStreamTrackPtr RTPReceiverTester::create(IMediaStreamTrackTypes::Kinds kind)
      {
        FakeMediaStreamTrackPtr track = FakeMediaStreamTrack::create(getAssociatedMessageQueue(), kind);
        track->setTransport(mThisWeak.lock());

        {
          AutoRecursiveLock lock(*this);

          TESTING_CHECK(!mMediaStreamTrack)

          mExpectations.mKind = kind;
          mMediaStreamTrack = track;
        }

        TESTING_CHECK(track)

        track->create(kind);

        return track;
      }

      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      //-----------------------------------------------------------------------
      #pragma mark
      #pragma mark RTPReceiverTester => (internal)
      #pragma mark

      //-----------------------------------------------------------------------
      Log::Params RTPReceiverTester::log(const char *message) const
      {
        ElementPtr objectEl = Element::create("ortc::test::rtpreceiver::RTPReceiverTester");
        UseServicesHelper::debugAppend(objectEl, "id", mID);
        return Log::Params(message, objectEl);
      }

      //-----------------------------------------------------------------------
      FakeICETransportPtr RTPReceiverTester::getICETransport() const
      {
        AutoRecursiveLock lock(*this);
        return mICETransport;
      }

      //-----------------------------------------------------------------------
      FakeReceiverChannelPtr RTPReceiverTester::getReceiverChannel(const char *receiverID)
      {
        AutoRecursiveLock lock(*this);
        auto found = mAttached.find(String(receiverID));
        if (mAttached.end() == found) return FakeReceiverChannelPtr();
        return (*found).second.first.lock();
      }

      //-----------------------------------------------------------------------
      FakeSenderPtr RTPReceiverTester::getSender(const char *senderID)
      {
        AutoRecursiveLock lock(*this);
        auto found = mAttached.find(String(senderID));
        if (mAttached.end() == found) return FakeSenderPtr();
        return (*found).second.second;
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::expectData(
                                         const char *senderOrReceiverID,
                                         SecureByteBlockPtr secureBuffer
                                         )
      {
        {
          FakeSenderPtr sender = getSender(senderOrReceiverID);

          if (sender) {
            sender->expectData(secureBuffer);
          }
        }

        {
          FakeReceiverChannelPtr receiverChannel = getReceiverChannel(senderOrReceiverID);

          if (receiverChannel) {
            receiverChannel->expectData(secureBuffer);
          }
        }
      }

      //-----------------------------------------------------------------------
      void RTPReceiverTester::sendData(
                                       const char *senderOrReceiverChannelID,
                                       SecureByteBlockPtr secureBuffer
                                       )
      {
        FakeSenderPtr sender = getSender(senderOrReceiverChannelID);
        TESTING_CHECK(sender)

        sender->sendPacket(secureBuffer);
      }
    }
  }
}

ZS_DECLARE_USING_PTR(ortc::test::rtpreceiver, FakeICETransport)
ZS_DECLARE_USING_PTR(ortc::test::rtpreceiver, RTPReceiverTester)
ZS_DECLARE_USING_PTR(ortc, IICETransport)
ZS_DECLARE_USING_PTR(ortc, IDTLSTransport)
ZS_DECLARE_USING_PTR(ortc, IDataChannel)
ZS_DECLARE_USING_PTR(ortc, IRTPReceiver)
ZS_DECLARE_USING_PTR(ortc, IRTPTypes)

ZS_DECLARE_USING_PTR(ortc::internal, RTPPacket)
ZS_DECLARE_USING_PTR(ortc::internal, RTCPPacket)
ZS_DECLARE_TYPEDEF_PTR(ortc::internal::ISecureTransportTypes, ISecureTransportTypes)

ZS_DECLARE_TYPEDEF_PTR(ortc::IRTPTypes::Parameters, Parameters)
ZS_DECLARE_TYPEDEF_PTR(ortc::IRTPTypes::CodecParameters, CodecParameters)
ZS_DECLARE_TYPEDEF_PTR(ortc::IRTPTypes::EncodingParameters, EncodingParameters)
ZS_DECLARE_TYPEDEF_PTR(ortc::IMediaStreamTrackTypes, IMediaStreamTrackTypes)

using ortc::IICETypes;
using zsLib::Optional;
using zsLib::WORD;
using zsLib::BYTE;
using zsLib::Milliseconds;
using ortc::SecureByteBlock;
using ortc::SecureByteBlockPtr;

#define TEST_BASIC_ROUTING 1
#define TEST_BASIC_ROUTING_EXTENDED_SOURCE 0
#define TEST_INCOMING_DELAYED_RTPReceiver 2

static void bogusSleep()
{
  for (int loop = 0; loop < 100; ++loop)
  {
    TESTING_SLEEP(100)
  }
}

void doTestRTPReceiver()
{
  typedef ortc::IRTPTypes IRTPTypes;


  if (!ORTC_TEST_DO_RTP_LISTENER_TEST) return;

  TESTING_INSTALL_LOGGER();

  TESTING_SLEEP(1000)

  ortc::ISettings::applyDefaults();

  zsLib::MessageQueueThreadPtr thread(zsLib::MessageQueueThread::createBasic());

  RTPReceiverTesterPtr testObject1;
  RTPReceiverTesterPtr testObject2;

  TESTING_STDOUT() << "WAITING:      Waiting for RTPReceiver testing to complete (max wait is 180 seconds).\n";

  // check to see if all DNS routines have resolved
  {
    ULONG testNumber = 0;
    ULONG maxSteps = 80;

    do
    {
      TESTING_STDOUT() << "TESTING       ---------->>>>>>>>>> " << testNumber << " <<<<<<<<<<----------\n";

      bool quit = false;
      ULONG expecting = 0;

      RTPReceiverTester::Expectations expectations1;
      RTPReceiverTester::Expectations expectations2;

      expectations2 = expectations1;

      switch (testNumber) {
        case TEST_BASIC_ROUTING: {
          {
            UseSettings::setUInt("ortc/rtp-receiver/max-age-rtp-packets-in-seconds", 60);
            UseSettings::setUInt("ortc/rtp-receiver/max-age-rtcp-packets-in-seconds", 60);

            testObject1 = RTPReceiverTester::create(thread);
            testObject2 = RTPReceiverTester::create(thread);

            TESTING_CHECK(testObject1)
            TESTING_CHECK(testObject2)

            testObject1->setClientRole(true);
            testObject2->setClientRole(false);

            expectations1.mUnhandled = 0;
            expectations1.mReceivedPackets = 1;
            expectations1.mChannelUpdate = 0;
            expectations1.mActiveReceiverChannel = 1;
            expectations1.mReceiverChannelOfSecureTransportState = 2;
            expectations1.mKind = IMediaStreamTrackTypes::Kind_Audio;
          }
          break;
        }
        default:  quit = true; break;
      }
      if (quit) break;

      expecting = 0;
      expecting += (testObject1 ? 1 : 0);
      expecting += (testObject2 ? 1 : 0);

      ULONG found = 0;
      ULONG lastFound = 0;
      ULONG step = 0;

      bool lastStepReached = false;

      while ((found < expecting) ||
             (!lastStepReached))
      {
        TESTING_SLEEP(1000)
        ++step;
        if (step >= maxSteps) {
          TESTING_CHECK(false)
          break;
        }

        found = 0;

        switch (testNumber) {
          case TEST_BASIC_ROUTING: {
            switch (step) {
              case 2: {
                if (testObject1) testObject1->connect(testObject2);
                bogusSleep();
                break;
              }
              case 3: {
                if (testObject1) testObject1->state(IICETransport::State_Completed);
                if (testObject2) testObject2->state(IICETransport::State_Completed);
                bogusSleep();
                break;
              }
              case 4: {
                if (testObject1) testObject1->state(IDTLSTransport::State_Validated);
                if (testObject2) testObject2->state(IDTLSTransport::State_Validated);
                bogusSleep();
                break;
              }
              case 5: {
                {
                  Parameters params;
                  testObject1->store("params-empty", params);
                }
                {
                  Parameters params;
                  params.mMuxID = "r1";

                  CodecParameters codec;
                  codec.mName = "opus";
                  codec.mClockRate = 48000;
                  codec.mPayloadType = 96;

                  params.mCodecs.push_back(codec);

                  IRTPTypes::HeaderExtensionParameters headerParams;
                  headerParams.mID = 1;
                  headerParams.mURI = IRTPTypes::toString(IRTPTypes::HeaderExtensionURI_MuxID);
                  params.mHeaderExtensions.push_back(headerParams);
                  headerParams.mID = 2;
                  headerParams.mURI = IRTPTypes::toString(IRTPTypes::HeaderExtensionURI_RID);
                  params.mHeaderExtensions.push_back(headerParams);

                  testObject1->store("param1", params);
                }
                bogusSleep();
                break;
              }
              case 6: {
                testObject1->createReceiverChannel("c1", "param1");
                testObject1->receive("param1");
                bogusSleep();
                break;
              }
              case 7: {
                Parameters params;
                testObject2->send("s1", "params-empty");
                bogusSleep();
                break;
              }
              case 8:
              {
                RTPPacket::CreationParams params;
                params.mPT = 96;
                params.mSequenceNumber = 1;
                params.mTimestamp = 10000;
                params.mSSRC = 5;
                const char *payload = "sparksthesoftware";
                params.mPayload = reinterpret_cast<const BYTE *>(payload);
                params.mPayloadSize = strlen(payload);

                RTPPacket::MidHeaderExtension mid1(1, "r1");
                params.mFirstHeaderExtension = &mid1;

                RTPPacket::MidHeaderExtension rid1(2, "c1");
                params.mFirstHeaderExtension->mNext = &rid1;

                RTPPacketPtr packet = RTPPacket::create(params);
                testObject1->store("p1", packet);
                testObject2->store("p1", packet);
                break;
              }
              case 9: {
                testObject1->expectState("c1", ISecureTransportTypes::State_Connected);
                testObject1->expectState("c1", ISecureTransportTypes::State_Closed);
                bogusSleep();
                break;
              }
              case 10: {
                testObject1->expectPacket("p1", "r1");
                bogusSleep();
                break;
              }

              case 16: {
                if (testObject1) testObject1->close();
                if (testObject2) testObject1->close();
                //bogusSleep();
                break;
              }
              case 17: {
                if (testObject1) testObject1->state(IDTLSTransport::State_Closed);
                if (testObject2) testObject2->state(IDTLSTransport::State_Closed);
                //bogusSleep();
                break;
              }
              case 18: {
                if (testObject1) testObject1->state(IICETransport::State_Disconnected);
                if (testObject2) testObject2->state(IICETransport::State_Disconnected);
                //bogusSleep();
                break;
              }
              case 19: {
                if (testObject1) testObject1->state(IICETransport::State_Closed);
                if (testObject2) testObject2->state(IICETransport::State_Closed);
                //bogusSleep();
                break;
              }
              case 20: {
                if (testObject1) testObject1->closeByReset();
                if (testObject2) testObject2->closeByReset();
                //bogusSleep();
                break;
              }
              case 21: {
                lastStepReached = true;
                break;
              }
              default: {
                // nothing happening in this step
                break;
              }
            }
            break;
          }
          default: {
            // none defined
            break;
          }
        }

        if (0 == found) {
          found += (testObject1 ? (testObject1->matches(expectations1) ? 1 : 0) : 0);
          found += (testObject2 ? (testObject2->matches(expectations2) ? 1 : 0) : 0);
        }

        if (lastFound != found) {
          lastFound = found;
          TESTING_STDOUT() << "FOUND:        [" << found << "].\n";
        }
      }

      TESTING_EQUAL(found, expecting)

      TESTING_SLEEP(2000)

      switch (testNumber) {
        default:
        {
          if (testObject1) {TESTING_CHECK(testObject1->matches(expectations1))}
          if (testObject2) {TESTING_CHECK(testObject2->matches(expectations2))}
          break;
        }
      }

      testObject1.reset();
      testObject2.reset();

      ++testNumber;
    } while (true);
  }

  TESTING_STDOUT() << "WAITING:      All RTPReceiver transports have finished. Waiting for 'bogus' events to process (1 second wait).\n";
  TESTING_SLEEP(1000)

  // wait for shutdown
  {
    IMessageQueue::size_type count = 0;
    do
    {
      count = thread->getTotalUnprocessedMessages();
      if (0 != count)
        std::this_thread::yield();
    } while (count > 0);

    thread->waitForShutdown();
  }
  TESTING_UNINSTALL_LOGGER();
  zsLib::proxyDump();
  TESTING_EQUAL(zsLib::proxyGetTotalConstructed(), 0);
}

