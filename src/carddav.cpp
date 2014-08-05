/*
 * This file is part of buteo-sync-plugin-carddav package
 *
 * Copyright (C) 2014 Jolla Ltd. and/or its subsidiary(-ies).
 *
 * Contributors: Chris Adams <chris.adams@jolla.com>
 *
 * This program/library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This program/library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program/library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "carddav_p.h"
#include "syncer_p.h"

#include <LogMacros.h>

#include <QRegularExpression>
#include <QUuid>
#include <QByteArray>
#include <QBuffer>
#include <QTimer>

#include <QContact>
#include <QContactGuid>

#include <QVersitWriter>
#include <QVersitDocument>
#include <QVersitProperty>
#include <QVersitContactExporter>
#include <QVersitReader>
#include <QVersitContactImporter>

namespace {
    void debugDumpData(const QString &data)
    {
        QString dbgout;
        Q_FOREACH (const QChar &c, data) {
            if (c == '\r' || c == '\n') {
                if (!dbgout.isEmpty()) {
                    LOG_DEBUG(dbgout);
                    dbgout.clear();
                }
            } else {
                dbgout += c;
            }
        }
        if (!dbgout.isEmpty()) {
            LOG_DEBUG(dbgout);
        }
    }
}

CardDavVCardConverter::CardDavVCardConverter()
{
}

CardDavVCardConverter::~CardDavVCardConverter()
{
}

QStringList CardDavVCardConverter::supportedPropertyNames()
{
    // We only support a small number of (core) vCard properties
    // in this sync adapter.  The rest of the properties will
    // be cached so that we can stitch them back into the vCard
    // we upload on modification.
    QStringList supportedProperties;
    supportedProperties << "VERSION" << "PRODID" << "REV"
                        << "N" << "FN" << "NICKNAME" << "BDAY" << "X-GENDER"
                        << "EMAIL" << "TEL" << "ADR" << "URL"
                        << "ORG" << "TITLE" << "ROLE"
                        << "UID";
    return supportedProperties;
}

QPair<QContact, QStringList> CardDavVCardConverter::convertVCardToContact(const QString &vcard, bool *ok)
{
    m_unsupportedProperties.clear();
    QVersitReader reader(vcard.toUtf8());
    reader.startReading();
    reader.waitForFinished();
    QList<QVersitDocument> vdocs = reader.results();
    if (vdocs.size() != 1) {
        LOG_WARNING(Q_FUNC_INFO
                   << "invalid results during vcard import, got"
                   << vdocs.size() << "output from input:\n" << vcard);
        *ok = false;
        return QPair<QContact, QStringList>();
    }

    // convert the vCard into a QContact
    QVersitContactImporter importer;
    importer.setPropertyHandler(this);
    importer.importDocuments(vdocs);
    QList<QContact> importedContacts = importer.contacts();
    if (importedContacts.size() != 1) {
        LOG_WARNING(Q_FUNC_INFO
                   << "invalid results during vcard conversion, got"
                   << importedContacts.size() << "output from input:\n" << vcard);
        *ok = false;
        return QPair<QContact, QStringList>();
    }

    QContact importedContact = importedContacts.first();
    QStringList unsupportedProperties = m_unsupportedProperties.value(importedContact.detail<QContactGuid>().guid());
    m_unsupportedProperties.clear();

    *ok = true;
    return qMakePair(importedContact, unsupportedProperties);
}

QString CardDavVCardConverter::convertContactToVCard(const QContact &c, const QStringList &unsupportedProperties)
{
    QList<QContact> exportList; exportList << c;
    QVersitContactExporter e;
    e.setDetailHandler(this);
    e.exportContacts(exportList);
    QByteArray output;
    QBuffer vCardBuffer(&output);
    vCardBuffer.open(QBuffer::WriteOnly);
    QVersitWriter writer(&vCardBuffer);
    writer.startWriting(e.documents());
    writer.waitForFinished();
    QString retn = QString::fromUtf8(output);

    // now add back the unsupported properties.
    Q_FOREACH (const QString &propStr, unsupportedProperties) {
        int endIdx = retn.lastIndexOf(QStringLiteral("END:VCARD"));
        if (endIdx > 0) {
            QString ecrlf = propStr + '\r' + '\n';
            retn.insert(endIdx, ecrlf);
        }
    }

/*
    LOG_DEBUG("generated vcard:");
    debugDumpData(retn);
*/

    return retn;
}

QString CardDavVCardConverter::convertPropertyToString(const QVersitProperty &p) const
{
    QVersitDocument d(QVersitDocument::VCard30Type);
    d.addProperty(p);
    QByteArray out;
    QBuffer bout(&out);
    bout.open(QBuffer::WriteOnly);
    QVersitWriter w(&bout);
    w.startWriting(d);
    w.waitForFinished();
    QString retn = QString::fromLatin1(out);

    // strip out the BEGIN:VCARD\r\nVERSION:3.0\r\n and END:VCARD\r\n\r\n bits.
    int headerIdx = retn.indexOf(QStringLiteral("VERSION:3.0")) + 11;
    int footerIdx = retn.indexOf(QStringLiteral("END:VCARD"));
    if (headerIdx > 11 && footerIdx > 0 && footerIdx > headerIdx) {
        retn = retn.mid(headerIdx, footerIdx - headerIdx).trimmed();
        return retn;
    }

    LOG_WARNING(Q_FUNC_INFO << "no string conversion possible for versit property:" << p.name());
    return QString();
}

void CardDavVCardConverter::propertyProcessed(const QVersitDocument &, const QVersitProperty &property,
                                               const QContact &, bool *alreadyProcessed,
                                               QList<QContactDetail> *updatedDetails)
{
    static QStringList supportedProperties(supportedPropertyNames());
    const QString propertyName(property.name().toUpper());
    if (supportedProperties.contains(propertyName)) {
        // do nothing, let the default handler import them.
        *alreadyProcessed = true;
        return;
    }

    // cache the unsupported property string, and remove any detail
    // which was added by the default handler for this property.
    *alreadyProcessed = true;
    QString unsupportedProperty = convertPropertyToString(property);
    m_tempUnsupportedProperties.append(unsupportedProperty);
    updatedDetails->clear();
}

void CardDavVCardConverter::documentProcessed(const QVersitDocument &, QContact *c)
{
    // the UID of the contact will be contained in the QContactGuid detail.
    QString uid = c->detail<QContactGuid>().guid();
    if (uid.isEmpty()) {
        LOG_WARNING(Q_FUNC_INFO << "imported contact has no UID, discarding unsupported properties!");
    } else {
        m_unsupportedProperties.insert(uid, m_tempUnsupportedProperties);
    }

    // get ready for the next import.
    m_tempUnsupportedProperties.clear();
}

void CardDavVCardConverter::contactProcessed(const QContact &, QVersitDocument *)
{
}

void CardDavVCardConverter::detailProcessed(const QContact &, const QContactDetail &,
                                            const QVersitDocument &, QSet<int> *,
                                            QList<QVersitProperty> *, QList<QVersitProperty> *toBeAdded)
{
    static QStringList supportedProperties(supportedPropertyNames());
    for (int i = toBeAdded->size() - 1; i >= 0; --i) {
        if (!supportedProperties.contains(toBeAdded->at(i).name().toUpper())) {
            // we don't support importing these properties, so we shouldn't
            // attempt to export them.
            toBeAdded->removeAt(i);
        }
    }
}

CardDav::CardDav(Syncer *parent,
                 const QString &serverUrl,
                 const QString &username,
                 const QString &password)
    : QObject(parent)
    , q(parent)
    , m_converter(new CardDavVCardConverter)
    , m_request(new RequestGenerator(q, username, password))
    , m_parser(new ReplyParser(q, m_converter))
    , m_serverUrl(serverUrl)
    , m_downsyncRequests(0)
    , m_upsyncRequests(0)
{
}

CardDav::CardDav(Syncer *parent,
                 const QString &serverUrl,
                 const QString &accessToken)
    : QObject(parent)
    , q(parent)
    , m_converter(new CardDavVCardConverter)
    , m_request(new RequestGenerator(q, accessToken))
    , m_parser(new ReplyParser(q, m_converter))
    , m_serverUrl(serverUrl)
    , m_downsyncRequests(0)
    , m_upsyncRequests(0)
{
}

CardDav::~CardDav()
{
    delete m_converter;
    delete m_parser;
    delete m_request;
}

void CardDav::errorOccurred()
{
    emit error();
}

void CardDav::determineRemoteAMR()
{
    // The CardDAV sequence for determining the A/M/R delta is:
    // a)  fetch user information from the principle URL
    // b)  fetch addressbooks home url
    // c)  fetch addressbook information
    // d)  for each addressbook, either:
    //     i)  perform immediate delta sync (if webdav-sync enabled) OR
    //     ii) fetch etags, manually calculate delta
    // e) fetch full contacts for delta.

    // We start by fetching user information.
    fetchUserInformation();
}

void CardDav::fetchUserInformation()
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting principle urls for user");
    QNetworkReply *reply = m_request->currentUserInformation(m_serverUrl);
    if (!reply) {
        emit error();
        return;
    }

    connect(reply, SIGNAL(finished()), this, SLOT(userInformationResponse()));
}

void CardDav::userInformationResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred();
        return;
    }

    QString userPath = m_parser->parseUserPrinciple(data);
    if (userPath.isEmpty()) {
        LOG_WARNING(Q_FUNC_INFO << "unable to parse user principle from response");
        emit error();
        return;
    }

    fetchAddressbookUrls(userPath);
}

void CardDav::fetchAddressbookUrls(const QString &userPath)
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting addressbook urls for user");
    QNetworkReply *reply = m_request->addressbookUrls(m_serverUrl, userPath);
    if (!reply) {
        emit error();
        return;
    }

    connect(reply, SIGNAL(finished()), this, SLOT(addressbookUrlsResponse()));
}

void CardDav::addressbookUrlsResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred();
        return;
    }

    QString addressbooksHomePath = m_parser->parseAddressbookHome(data);
    if (addressbooksHomePath.isEmpty()) {
        LOG_WARNING(Q_FUNC_INFO << "unable to parse addressbook home from response");
        emit error();
        return;
    }

    fetchAddressbooksInformation(addressbooksHomePath);
}

void CardDav::fetchAddressbooksInformation(const QString &addressbooksHomePath)
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting addressbook sync information");
    QNetworkReply *reply = m_request->addressbooksInformation(m_serverUrl, addressbooksHomePath);
    if (!reply) {
        emit error();
        return;
    }

    connect(reply, SIGNAL(finished()), this, SLOT(addressbooksInformationResponse()));
}

void CardDav::addressbooksInformationResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred();
        return;
    }

    QList<ReplyParser::AddressBookInformation> infos = m_parser->parseAddressbookInformation(data);
    if (infos.isEmpty()) {
        LOG_WARNING(Q_FUNC_INFO << "unable to parse addressbook info from response");
        emit error();
        return;
    }

    // for addressbooks which support sync-token syncing, use that style.
    for (int i = 0; i < infos.size(); ++i) {
        // set a default addressbook if we haven't seen one yet.
        // we will store newly added local contacts to that addressbook.
        if (q->m_defaultAddressbook.isEmpty()) {
            q->m_defaultAddressbook = infos[i].url;
        }

        if (infos[i].syncToken.isEmpty()) {
            // we cannot use sync-token for this addressbook, but instead ctag.
            const QString &existingCtag(q->m_addressbookCtags[infos[i].url]); // from OOB
            if (existingCtag.isEmpty()) {
                // first time sync
                q->m_addressbookCtags[infos[i].url] = infos[i].ctag; // insert
                // now do etag request, the delta will be all remote additions
                fetchContactMetadata(infos[i].url);
            } else if (existingCtag != infos[i].ctag) {
                // changes have occurred since last sync
                q->m_addressbookCtags[infos[i].url] = infos[i].ctag; // update
                // perform etag request and then manually calculate deltas.
                fetchContactMetadata(infos[i].url);
            } else {
                // no changes have occurred in this addressbook since last sync
                LOG_DEBUG(Q_FUNC_INFO << "no changes since last sync for"
                         << infos[i].url << "from account" << q->m_accountId);
                m_downsyncRequests += 1;
                QTimer::singleShot(0, this, SLOT(downsyncComplete()));
            }
        } else {
            // the server supports webdav-sync for this addressbook.
            const QString &existingSyncToken(q->m_addressbookSyncTokens[infos[i].url]); // from OOB
            // store the ctag anyway just in case the server has
            // forgotten the syncToken we cached from last time.
            if (!infos[i].ctag.isEmpty()) {
                q->m_addressbookCtags[infos[i].url] = infos[i].ctag;
            }
            // attempt to perform synctoken sync
            if (existingSyncToken.isEmpty()) {
                // first time sync
                q->m_addressbookSyncTokens[infos[i].url] = infos[i].syncToken; // insert
                // perform slow sync / full report
                fetchContactMetadata(infos[i].url);
            } else if (existingSyncToken != infos[i].syncToken) {
                // changes have occurred since last sync.
                q->m_addressbookSyncTokens[infos[i].url] = infos[i].syncToken; // update
                // perform immediate delta sync, by passing the old sync token to the server.
                fetchImmediateDelta(infos[i].url, existingSyncToken);
            } else {
                // no changes have occurred in this addressbook since last sync
                LOG_DEBUG(Q_FUNC_INFO << "no changes since last sync for"
                         << infos[i].url << "from account" << q->m_accountId);
                m_downsyncRequests += 1;
                QTimer::singleShot(0, this, SLOT(downsyncComplete()));
            }
        }
    }
}

void CardDav::fetchImmediateDelta(const QString &addressbookUrl, const QString &syncToken)
{
    LOG_DEBUG(Q_FUNC_INFO
             << "requesting immediate delta for addressbook" << addressbookUrl
             << "with sync token" << syncToken);

    QNetworkReply *reply = m_request->syncTokenDelta(m_serverUrl, addressbookUrl, syncToken);
    if (!reply) {
        emit error();
        return;
    }

    m_downsyncRequests += 1; // when this reaches zero, we've finished all addressbook deltas
    reply->setProperty("addressbookUrl", addressbookUrl);
    connect(reply, SIGNAL(finished()), this, SLOT(immediateDeltaResponse()));
}

void CardDav::immediateDeltaResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString addressbookUrl = reply->property("addressbookUrl").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << ")");
        debugDumpData(QString::fromUtf8(data));
        // The server is allowed to forget the syncToken by the
        // carddav protocol.  Try a full report sync just in case.
        fetchContactMetadata(addressbookUrl);
        return;
    }

    QString newSyncToken;
    QList<ReplyParser::ContactInformation> infos = m_parser->parseSyncTokenDelta(data, &newSyncToken);
    q->m_addressbookSyncTokens[addressbookUrl] = newSyncToken;
    fetchContacts(addressbookUrl, infos);
}

void CardDav::fetchContactMetadata(const QString &addressbookUrl)
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting contact metadata for addressbook" << addressbookUrl);
    QNetworkReply *reply = m_request->contactEtags(m_serverUrl, addressbookUrl);
    if (!reply) {
        emit error();
        return;
    }

    m_downsyncRequests += 1; // when this reaches zero, we've finished all addressbook deltas
    reply->setProperty("addressbookUrl", addressbookUrl);
    connect(reply, SIGNAL(finished()), this, SLOT(contactMetadataResponse()));
}

void CardDav::contactMetadataResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString addressbookUrl = reply->property("addressbookUrl").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred();
        return;
    }

    QList<ReplyParser::ContactInformation> infos = m_parser->parseContactMetadata(data, addressbookUrl);
    fetchContacts(addressbookUrl, infos);
}

void CardDav::fetchContacts(const QString &addressbookUrl, const QList<ReplyParser::ContactInformation> &amrInfo)
{
    LOG_DEBUG(Q_FUNC_INFO << "requesting full contact information from addressbook" << addressbookUrl);

    // split into A/M/R request sets
    QStringList contactUris;
    Q_FOREACH (const ReplyParser::ContactInformation &info, amrInfo) {
        if (info.modType == ReplyParser::ContactInformation::Addition) {
            q->m_serverAdditionIndices[addressbookUrl].insert(info.uri, q->m_serverAdditions[addressbookUrl].size());
            q->m_serverAdditions[addressbookUrl].append(info);
            contactUris.append(info.uri);
        } else if (info.modType == ReplyParser::ContactInformation::Modification) {
            q->m_serverModificationIndices[addressbookUrl].insert(info.uri, q->m_serverModifications[addressbookUrl].size());
            q->m_serverModifications[addressbookUrl].append(info);
            contactUris.append(info.uri);
        } else if (info.modType == ReplyParser::ContactInformation::Deletion) {
            q->m_serverDeletions[addressbookUrl].append(info);
        } else {
            LOG_WARNING(Q_FUNC_INFO << "no modification type in info for:" << info.uri);
        }
    }

    LOG_DEBUG(Q_FUNC_INFO << "Have calculated AMR:"
             << q->m_serverAdditions[addressbookUrl].size()
             << q->m_serverModifications[addressbookUrl].size()
             << q->m_serverDeletions[addressbookUrl].size()
             << "for addressbook:" << addressbookUrl);

    if (contactUris.isEmpty()) {
        // no additions or modifications to fetch.
        LOG_DEBUG(Q_FUNC_INFO << "no further data to fetch");
        contactAddModsComplete(addressbookUrl);
    } else {
        // fetch the full contact data for additions/modifications.
        LOG_DEBUG(Q_FUNC_INFO << "fetching vcard data for" << contactUris.size() << "contacts");
        QNetworkReply *reply = m_request->contactMultiget(m_serverUrl, addressbookUrl, contactUris);
        if (!reply) {
            emit error();
            return;
        }

        reply->setProperty("addressbookUrl", addressbookUrl);
        connect(reply, SIGNAL(finished()), this, SLOT(contactsResponse()));
    }
}

void CardDav::contactsResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString addressbookUrl = reply->property("addressbookUrl").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred();
        return;
    }

    QList<QContact> added;
    QList<QContact> modified;

    // fill out added/modified.  Also keep our addressbookContactGuids state up-to-date.
    // The addMods map is a map from server contact uri to <contact/unsupportedProperties/etag>.
    QMap<QString, ReplyParser::FullContactInformation> addMods = m_parser->parseContactData(data);
    QMap<QString, ReplyParser::FullContactInformation>::const_iterator it = addMods.constBegin();
    for ( ; it != addMods.constEnd(); ++it) {
        if (q->m_serverAdditionIndices[addressbookUrl].contains(it.key())) {
            QString guid = it.value().contact.detail<QContactGuid>().guid();
            q->m_serverAdditions[addressbookUrl][q->m_serverAdditionIndices[addressbookUrl].value(it.key())].guid = guid;
            q->m_contactEtags[guid] = it.value().etag;
            q->m_contactUris[guid] = it.key();
            q->m_contactUnsupportedProperties[guid] = it.value().unsupportedProperties;
            // Note: for additions, q->m_contactUids will have been filled out by the reply parser.
            q->m_addressbookContactGuids[addressbookUrl].append(guid);
            // Check to see if this server-side addition is actually just
            // a reported previously-upsynced local-side addition.
            if (q->m_contactIds.contains(guid)) {
                QContact previouslyUpsynced = it.value().contact;
                previouslyUpsynced.setId(QContactId::fromString(q->m_contactIds[guid]));
                added.append(previouslyUpsynced);
            } else {
                // pure server-side addition.
                added.append(it.value().contact);
            }
        } else if (q->m_serverModificationIndices[addressbookUrl].contains(it.key())) {
            QContact c = it.value().contact;
            QString guid = c.detail<QContactGuid>().guid();
            q->m_contactUnsupportedProperties[guid] = it.value().unsupportedProperties;
            q->m_contactEtags[guid] = it.value().etag;
            if (!q->m_contactIds.contains(guid)) {
                LOG_WARNING(Q_FUNC_INFO << "modified contact has no id");
            } else {
                c.setId(QContactId::fromString(q->m_contactIds[guid]));
            }
            modified.append(c);
        } else {
            LOG_WARNING(Q_FUNC_INFO << "ignoring unknown addition/modification:" << it.key());
        }
    }

    // coalesce the added/modified contacts from this addressbook into the complete AMR
    m_remoteAdditions.append(added);
    m_remoteModifications.append(modified);

    // now handle removals
    contactAddModsComplete(addressbookUrl);
}

void CardDav::contactAddModsComplete(const QString &addressbookUrl)
{
    QList<QContact> removed;

    // fill out removed set, and remove any state data associated with removed contacts
    for (int i = 0; i < q->m_serverDeletions[addressbookUrl].size(); ++i) {
        QString guid = q->m_serverDeletions[addressbookUrl][i].guid;

        // create the contact to remove
        QContact doomed;
        QContactGuid cguid;
        cguid.setGuid(guid);
        doomed.saveDetail(&cguid);
        if (!q->m_contactIds.contains(guid)) {
            LOG_WARNING(Q_FUNC_INFO << "removed contact has no id");
            continue; // cannot remove it if we don't know the id
        }
        doomed.setId(QContactId::fromString(q->m_contactIds[guid]));
        removed.append(doomed);

        // update the state data
        q->m_contactUids.remove(guid);
        q->m_contactUris.remove(guid);
        q->m_contactEtags.remove(guid);
        q->m_contactIds.remove(guid);
        q->m_contactUnsupportedProperties.remove(guid);
        q->m_addressbookContactGuids[addressbookUrl].removeOne(guid);
    }

    // coalesce the removed contacts from this addressbook into the complete AMR
    m_remoteRemovals.append(removed);

    // downsync complete for this addressbook.
    // we use a singleshot to ensure that the m_deltaRequests count isn't
    // decremented synchronously to zero if the first addressbook didn't
    // have any remote additions or modifications (requiring async request).
    QTimer::singleShot(0, this, SLOT(downsyncComplete()));
}

void CardDav::downsyncComplete()
{
    // downsync complete for this addressbook
    // if this was the last outstanding addressbook, we're finished.
    m_downsyncRequests -= 1;
    if (m_downsyncRequests == 0) {
        LOG_DEBUG(Q_FUNC_INFO
                 << "downsync complete with total AMR:"
                 << m_remoteAdditions.size() << ","
                 << m_remoteModifications.size() << ","
                 << m_remoteRemovals.size());
        emit remoteChanges(m_remoteAdditions, m_remoteModifications, m_remoteRemovals);
    }
}

void CardDav::upsyncUpdates(const QString &addressbookUrl, const QList<QContact> &added, const QList<QContact> &modified, const QList<QContact> &removed)
{
    LOG_DEBUG(Q_FUNC_INFO
             << "upsyncing updates to addressbook:" << addressbookUrl
             << ":" << added.count() << modified.count() << removed.count());

    if (added.size() == 0 && modified.size() == 0 && removed.size() == 0) {
        // nothing to upsync.  Use a singleshot to avoid synchronously
        // decrementing the m_upsyncRequests count to zero if there
        // happens to be nothing to upsync to the first addressbook.
        m_upsyncRequests += 1;
        QTimer::singleShot(0, this, SLOT(upsyncComplete()));
    } else {
        // put local additions
        for (int i = 0; i < added.size(); ++i) {
            QContact c = added.at(i);
            // generate a server-side uid
            QString uid = QUuid::createUuid().toString().replace(QRegularExpression(QStringLiteral("[\\-{}]")), QString());
            // transform into local-device guid
            QString guid = QStringLiteral("%1:%2").arg(q->m_accountId).arg(uid);
            // generate a valid uri
            QString uri = addressbookUrl + "/" + uid + ".vcf";
            // update our state data
            q->m_contactUids[guid] = uid;
            q->m_contactUris[guid] = uri;
            q->m_contactIds[guid] = c.id().toString();
            // set the uid not guid so that the UID is generated.
            QContactGuid cguid = c.detail<QContactGuid>();
            cguid.setGuid(uid);
            c.saveDetail(&cguid);
            // generate a vcard
            QString vcard = m_converter->convertContactToVCard(c, QStringList());
            // upload
            QNetworkReply *reply = m_request->upsyncAddMod(m_serverUrl, uri, QString(), vcard);
            if (!reply) {
                emit error();
                return;
            }

            m_upsyncRequests += 1;
            reply->setProperty("addressbookUrl", addressbookUrl);
            reply->setProperty("contactGuid", guid);
            connect(reply, SIGNAL(finished()), this, SLOT(upsyncResponse()));
        }

        // put local modifications
        for (int i = 0; i < modified.size(); ++i) {
            QContact c = modified.at(i);
            // reinstate the server-side UID into the guid detail
            QContactGuid cguid = c.detail<QContactGuid>();
            QString guidstr = c.detail<QContactGuid>().guid();
            if (guidstr.isEmpty()) {
                LOG_WARNING(Q_FUNC_INFO << "modified contact has no guid:" << c.id().toString());
                continue; // TODO: this is actually an error.
            }
            QString uidstr = q->m_contactUids[guidstr];
            if (uidstr.isEmpty()) {
                LOG_WARNING(Q_FUNC_INFO << "modified contact server uid unknown:" << c.id().toString() << guidstr);
                continue; // TODO: this is actually an error.
            }
            cguid.setGuid(uidstr);
            c.saveDetail(&cguid);
            QString vcard = m_converter->convertContactToVCard(c, q->m_contactUnsupportedProperties[guidstr]);
            // upload
            QNetworkReply *reply = m_request->upsyncAddMod(m_serverUrl,
                    q->m_contactUris[guidstr],
                    q->m_contactEtags[guidstr],
                    vcard);
            if (!reply) {
                emit error();
                return;
            }

            m_upsyncRequests += 1;
            reply->setProperty("addressbookUrl", addressbookUrl);
            reply->setProperty("contactGuid", guidstr);
            connect(reply, SIGNAL(finished()), this, SLOT(upsyncResponse()));
        }

        // delete local removals
        for (int i = 0; i < removed.size(); ++i) {
            const QString &guidstr(removed[i].detail<QContactGuid>().guid());
            QNetworkReply *reply = m_request->upsyncDeletion(m_serverUrl,
                    q->m_contactUris[guidstr],
                    q->m_contactEtags[guidstr]);
            if (!reply) {
                emit error();
                return;
            }

            // clear state data for this (deleted) contact
            q->m_contactEtags.remove(guidstr);
            q->m_contactUris.remove(guidstr);
            q->m_contactIds.remove(guidstr);
            q->m_contactUids.remove(guidstr);
            q->m_addressbookContactGuids[addressbookUrl].removeOne(guidstr);

            m_upsyncRequests += 1;
            reply->setProperty("addressbookUrl", addressbookUrl);
            connect(reply, SIGNAL(finished()), this, SLOT(upsyncResponse()));
        }
    }
}

void CardDav::upsyncResponse()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QString guid = reply->property("contactGuid").toString();
    QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        LOG_WARNING(Q_FUNC_INFO << "error:" << reply->error()
                   << "(" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << ")");
        debugDumpData(QString::fromUtf8(data));
        errorOccurred();
        return;
    }

    if (!guid.isEmpty()) {
        // this is an addition or modification.
        // get the new etag value reported by the server.
        QString etag;
        Q_FOREACH(const QByteArray &header, reply->rawHeaderList()) {
            if (QString::fromUtf8(header).contains(QLatin1String("etag"), Qt::CaseInsensitive)) {
                etag = reply->rawHeader(header);
                break;
            }
        }

        if (!etag.isEmpty()) {
            q->m_contactEtags[guid] = etag;
        }
    }

    // upsync is complete for this addressbook.
    upsyncComplete();
}

void CardDav::upsyncComplete()
{
    m_upsyncRequests -= 1;
    if (m_upsyncRequests == 0) {
        // finished upsyncing all data for all addressbooks.
        LOG_DEBUG(Q_FUNC_INFO << "upsync complete");
        emit upsyncCompleted();
    }
}

