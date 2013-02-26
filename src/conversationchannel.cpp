/* Copyright (C) 2013 Jolla Ltd
 * Copyright (C) 2012 John Brooks <john.brooks@dereferenced.net>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "conversationchannel.h"
#include "clienthandler.h"

#include <TelepathyQt/ChannelRequest>
#include <TelepathyQt/TextChannel>
#include <TelepathyQt/Channel>
#include <TelepathyQt/PendingReady>
#include <TelepathyQt/Contact>
#include <TelepathyQt/Account>

ConversationChannel::ConversationChannel(const QString &localUid, const QString &remoteUid, QObject *parent)
    : QObject(parent), mPendingRequest(0), mState(Null), mLocalUid(localUid), mRemoteUid(remoteUid)
{
}

ConversationChannel::~ConversationChannel()
{
}

// XXX all of these should be on the new model...
#if 0
void ConversationChannel::setupGroup(const CommHistory::Group &group)
{
    mGroupId = group.id();

    mContactId = group.remoteUids().value(0);
    mLocalUid = group.localUid();
    emit contactIdChanged();

    Q_ASSERT(!mModel);
    mModel = new CommHistory::ConversationModel(this);
    mModel->setBackgroundThread(GroupManager::instance()->groupModel()->backgroundThread());
    mModel->setQueryMode(CommHistory::EventModel::StreamedAsyncQuery);
    mModel->setFirstChunkSize(25);
    mModel->setChunkSize(50);
    mModel->setTreeMode(false);
    mModel->getEvents(mGroupId);
    emit chatModelReady(mModel);
}
#endif

void ConversationChannel::ensureChannel()
{
    if (!mChannel.isNull() || mPendingRequest || !mRequest.isNull())
        return;

    mAccount = Tp::Account::create(TP_QT_ACCOUNT_MANAGER_BUS_NAME, mLocalUid);
    if (!mAccount) {
        qWarning() << "ConversationChannel::ensureChannel no account for" << mLocalUid;
        setState(Error);
        return;
    }

    if (mAccount->isReady()) {
        accountReadyForChannel(0);
    } else {
        connect(mAccount->becomeReady(), SIGNAL(finished(Tp::PendingOperation*)),
                SLOT(accountReadyForChannel(Tp::PendingOperation*)));
    }
}

void ConversationChannel::accountReadyForChannel(Tp::PendingOperation *op)
{
    if (op && op->isError()) {
        qWarning() << "No account for" << mLocalUid;
        setState(Error);
        return;
    }

    Tp::PendingChannelRequest *req = mAccount->ensureTextChat(mRemoteUid,
            QDateTime::currentDateTime(),
            QLatin1String("org.freedesktop.Telepathy.Client.qmlmessages"));
    start(req);
}

void ConversationChannel::start(Tp::PendingChannelRequest *pendingRequest)
{
    Q_ASSERT(state() == Null || state() == Error);
    Q_ASSERT(!mPendingRequest);
    if (state() != Null && state() != Error)
        return;

    mPendingRequest = pendingRequest;
    connect(mPendingRequest, SIGNAL(channelRequestCreated(Tp::ChannelRequestPtr)),
            SLOT(channelRequestCreated(Tp::ChannelRequestPtr)));

    setState(PendingRequest);
}

void ConversationChannel::setChannel(const Tp::ChannelPtr &c)
{
    if (mChannel && c && mChannel->objectPath() == c->objectPath())
        return;
    if (!mChannel.isNull() || c.isNull()) {
        qWarning() << Q_FUNC_INFO << "called with existing channel set";
        return;
    }

    mChannel = c;
    connect(mChannel->becomeReady(Tp::TextChannel::FeatureMessageQueue),
            SIGNAL(finished(Tp::PendingOperation*)),
            SLOT(channelReady()));
    connect(mChannel.data(), SIGNAL(messageReceived(Tp::ReceivedMessage)),
            SLOT(messageReceived(Tp::ReceivedMessage)));
    connect(mChannel.data(), SIGNAL(invalidated(Tp::DBusProxy*,QString,QString)),
            SLOT(channelInvalidated(Tp::DBusProxy*,QString,QString)));

    setState(PendingReady);
}

void ConversationChannel::channelRequestCreated(const Tp::ChannelRequestPtr &r)
{
    Q_ASSERT(state() == PendingRequest);
    Q_ASSERT(mRequest.isNull());
    Q_ASSERT(!r.isNull());
    if (state() != PendingRequest)
        return;

    qDebug() << Q_FUNC_INFO;

    mRequest = r;
    connect(mRequest.data(), SIGNAL(succeeded(Tp::ChannelPtr)),
            SLOT(channelRequestSucceeded(Tp::ChannelPtr)));
    connect(mRequest.data(), SIGNAL(failed(QString,QString)),
            SLOT(channelRequestFailed(QString,QString)));

    mPendingRequest = 0;
    setState(Requested);
}

void ConversationChannel::channelRequestSucceeded(const Tp::ChannelPtr &channel)
{
    Q_ASSERT(state() == Requested);
    Q_ASSERT(!mRequest.isNull());
    // Telepathy docs note that channel may be null if the dispatcher is too old.
    Q_ASSERT(!channel.isNull());
    if (channel.isNull()) {
        qWarning() << Q_FUNC_INFO << "channel is null (dispatcher too old?)";
        return;
    }

    qDebug() << Q_FUNC_INFO;
    setChannel(channel);
    mRequest.reset();
    emit requestSucceeded();
}

void ConversationChannel::channelRequestFailed(const QString &errorName,
        const QString &errorMessage)
{
    Q_ASSERT(state() == Requested);

    mRequest.reset();
    setState(Error);
    emit requestFailed(errorName, errorMessage);

    qDebug() << Q_FUNC_INFO << errorName << errorMessage;
}

void ConversationChannel::channelReady()
{
    Q_ASSERT(state() == PendingReady);
    Q_ASSERT(!mChannel.isNull());
    if (state() != PendingReady || mChannel.isNull())
        return;

    Tp::TextChannelPtr textChannel = Tp::SharedPtr<Tp::TextChannel>::dynamicCast(mChannel);
    Q_ASSERT(!textChannel.isNull());

    setState(Ready);

    if (!mPendingMessages.isEmpty())
        qDebug() << Q_FUNC_INFO << "Sending buffered messages";
    foreach (QString msg, mPendingMessages)
        textChannel->send(msg);
    mPendingMessages.clear();

    // Blindly acknowledge all messages, assuming commhistory handled them
    if (!textChannel->messageQueue().isEmpty())
        textChannel->acknowledge(textChannel->messageQueue());
}

void ConversationChannel::setState(State newState)
{
    if (mState == newState)
        return;

    mState = newState;
    emit stateChanged(newState);
}

void ConversationChannel::messageReceived(const Tp::ReceivedMessage &message)
{
    if (mChannel.isNull())
        return;

    Tp::TextChannelPtr textChannel = Tp::SharedPtr<Tp::TextChannel>::dynamicCast(mChannel);
    Q_ASSERT(!textChannel.isNull());

    textChannel->acknowledge(QList<Tp::ReceivedMessage>() << message);
}

void ConversationChannel::sendMessage(const QString &text)
{
    if (mChannel.isNull() || !mChannel->isReady()) {
        Q_ASSERT(state() != Ready);
        ensureChannel();
        qDebug() << Q_FUNC_INFO << "Buffering:" << text;
        mPendingMessages.append(text);
        return;
    }

    Tp::TextChannelPtr textChannel = Tp::SharedPtr<Tp::TextChannel>::dynamicCast(mChannel);
    Q_ASSERT(!textChannel.isNull());
    if (textChannel.isNull()) {
        qWarning() << Q_FUNC_INFO << "Channel is not a text channel; cannot send messages";
        return;
    }

    // Note that buffered messages do not use this path. See channelReady.
    qDebug() << Q_FUNC_INFO << text;
    Tp::PendingSendMessage *msg = textChannel->send(text);
}

void ConversationChannel::channelInvalidated(Tp::DBusProxy *proxy,
        const QString &errorName, const QString &errorMessage)
{
    Q_UNUSED(proxy);
    qDebug() << "Channel invalidated:" << errorName << errorMessage;

    mChannel.reset();
    setState(Null);
}

