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

#include "replyparser_p.h"
#include "syncer_p.h"
#include "carddav_p.h"

#include <LogMacros.h>

#include <QString>
#include <QList>
#include <QXmlStreamReader>
#include <QByteArray>
#include <QRegularExpression>

#include <QContactGuid>

namespace {
    void debugDumpData(const QString &data)
    {
        if (Buteo::Logger::instance()->getLogLevel() < 7) {
            return;
        }

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

    QVariantMap elementToVMap(QXmlStreamReader &reader)
    {
        QVariantMap element;

        // store the attributes of the element
        QXmlStreamAttributes attrs = reader.attributes();
        while (attrs.size()) {
            QXmlStreamAttribute attr = attrs.takeFirst();
            element.insert(attr.name().toString(), attr.value().toString());
        }

        while (reader.readNext() != QXmlStreamReader::EndElement) {
            if (reader.isCharacters()) {
                // store the text of the element, if any
                QString elementText = reader.text().toString();
                if (!elementText.isEmpty()) {
                    element.insert(QLatin1String("@text"), elementText);
                }
            } else if (reader.isStartElement()) {
                // recurse if necessary.
                QString subElementName = reader.name().toString();
                QVariantMap subElement = elementToVMap(reader);
                if (element.contains(subElementName)) {
                    // already have an element with this name.
                    // create a variantlist and append.
                    QVariant existing = element.value(subElementName);
                    QVariantList subElementList;
                    if (existing.type() == QVariant::Map) {
                        // we need to convert the value into a QVariantList
                        subElementList << existing.toMap();
                    } else if (existing.type() == QVariant::List) {
                        subElementList = existing.toList();
                    }
                    subElementList << subElement;
                    element.insert(subElementName, subElementList);
                } else {
                    // first element with this name.  insert as a map.
                    element.insert(subElementName, subElement);
                }
            }
        }

        return element;
    }

    QVariantMap xmlToVMap(QXmlStreamReader &reader)
    {
        QVariantMap retn;
        while (!reader.atEnd() && !reader.hasError() && reader.readNextStartElement()) {
            QString elementName = reader.name().toString();
            QVariantMap element = elementToVMap(reader);
            retn.insert(elementName, element);
        }
        return retn;
    }
}

ReplyParser::ReplyParser(Syncer *parent, CardDavVCardConverter *converter)
    : q(parent), m_converter(converter)
{
}

ReplyParser::~ReplyParser()
{
}

QString ReplyParser::parseUserPrincipal(const QByteArray &userInformationResponse, ReplyParser::ResponseType *responseType) const
{
    /* We expect a response of the form:
        HTTP/1.1 207 Multi-status
        Content-Type: application/xml; charset=utf-8

        <d:multistatus xmlns:d="DAV:">
            <d:response>
                <d:href>/</d:href>
                <d:propstat>
                    <d:prop>
                        <d:current-user-principal>
                            <d:href>/principals/users/johndoe/</d:href>
                        </d:current-user-principal>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
        </d:multistatus>

      Note however that some CardDAV servers return addressbook
      information instead of user principal information.
    */
    debugDumpData(QString::fromUtf8(userInformationResponse));
    QXmlStreamReader reader(userInformationResponse);
    QVariantMap vmap = xmlToVMap(reader);
    QVariantMap multistatusMap = vmap[QLatin1String("multistatus")].toMap();
    if (multistatusMap[QLatin1String("response")].type() == QVariant::List) {
        // This should not be the case for a UserPrincipal response.
        *responseType = ReplyParser::AddressbookInformationResponse;
        return QString();
    }

    // Only one response - this could be either a UserPrincipal response
    // or an AddressbookInformation response.
    QVariantMap response = multistatusMap[QLatin1String("response")].toMap();
    QString statusText = response.value("propstat").toMap().value("status").toMap().value("@text").toString();
    QString userPrincipal = response.value("propstat").toMap().value("prop").toMap()
            .value("current-user-principal").toMap().value("href").toMap().value("@text").toString();
    QString ctag = response.value("propstat").toMap().value("prop").toMap().value("getctag").toMap().value("@text").toString();

    if (!statusText.contains(QLatin1String("200 OK"))) {
        LOG_WARNING(Q_FUNC_INFO << "invalid status response to current user information request:" << statusText);
    } else if (userPrincipal.isEmpty() && !ctag.isEmpty()) {
        // this server has responded with an addressbook information response.
        LOG_DEBUG(Q_FUNC_INFO << "addressbook information response to current user information request:" << statusText);
        *responseType = ReplyParser::AddressbookInformationResponse;
        return QString();
    }

    *responseType = ReplyParser::UserPrincipalResponse;
    return userPrincipal;
}

QString ReplyParser::parseAddressbookHome(const QByteArray &addressbookUrlsResponse) const
{
    /* We expect a response of the form:
        HTTP/1.1 207 Multi-status
        Content-Type: application/xml; charset=utf-8

        <d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
            <d:response>
                <d:href>/</d:href>
                <d:propstat>
                    <d:prop>
                        <c:addressbook-home-set>
                            <d:href>/addressbooks/johndoe/</d:href>
                        </c:addressbook-home-set>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
        </d:multistatus>
    */
    debugDumpData(QString::fromUtf8(addressbookUrlsResponse));
    QXmlStreamReader reader(addressbookUrlsResponse);
    QString statusText;
    QString addressbookHome;

    while (!reader.atEnd() && !reader.hasError()) {
        QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (reader.name().toString() == QLatin1String("addressbook-home-set")) {
                if (reader.readNextStartElement() && reader.name().toString() == QLatin1String("href")) {
                    addressbookHome = reader.readElementText();
                }
            } else if (reader.name().toString() == QLatin1String("status")) {
                statusText = reader.readElementText();
            }
        }
    }

    if (!statusText.contains(QLatin1String("200 OK"))) {
        LOG_WARNING(Q_FUNC_INFO << "invalid status response to addressbook home request:" << statusText);
    }

    return addressbookHome;
}

QList<ReplyParser::AddressBookInformation> ReplyParser::parseAddressbookInformation(const QByteArray &addressbookInformationResponse) const
{
    /* We expect a response of the form:
        <d:multistatus xmlns:d="DAV:" xmlns:cs="http://calendarserver.org/ns/">
            <d:response>
                <d:href>/addressbooks/johndoe/contacts/</d:href>
                <d:propstat>
                    <d:prop>
                        <d:displayname>My Address Book</d:displayname>
                        <cs:getctag>3145</cs:getctag>
                        <d:sync-token>http://sabredav.org/ns/sync-token/3145</d:sync-token>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
        </d:multistatus>
    */
    debugDumpData(QString::fromUtf8(addressbookInformationResponse));
    QXmlStreamReader reader(addressbookInformationResponse);
    QList<ReplyParser::AddressBookInformation> infos;

    QVariantMap vmap = xmlToVMap(reader);
    QVariantMap multistatusMap = vmap[QLatin1String("multistatus")].toMap();
    QVariantList responses;
    if (multistatusMap[QLatin1String("response")].type() == QVariant::List) {
        // multiple addressbooks.
        responses = multistatusMap[QLatin1String("response")].toList();
    } else {
        // only one addressbook.
        QVariantMap response = multistatusMap[QLatin1String("response")].toMap();
        responses << response;
    }

    // parse the information about each addressbook (response element)
    Q_FOREACH (const QVariant &rv, responses) {
        QVariantMap rmap = rv.toMap();
        ReplyParser::AddressBookInformation currInfo;
        currInfo.url = QUrl::fromPercentEncoding(rmap.value("href").toMap().value("@text").toString().toUtf8());

        // some services (e.g. Cozy) return multiple propstat elements in each response
        QVariantList propstats;
        if (rmap.value("propstat").type() == QVariant::List) {
            propstats = rmap.value("propstat").toList();
        } else {
            QVariantMap propstat = rmap.value("propstat").toMap();
            propstats << propstat;
        }

        // examine the propstat elements to find the features we're interested in
        enum ResourceStatus { StatusUnknown = 0,
                              StatusExplicitly2xxOk = 1,
                              StatusExplicitlyTrue = 1,
                              StatusExplicitlyNotOk = 2,
                              StatusExplicitlyFalse = 2 };
        ResourceStatus addressbookResourceSpecified = StatusUnknown; // valid values are Unknown/True/False
        ResourceStatus resourcetypeStatus = StatusUnknown;  // valid values are Unknown/2xxOk/NotOk
        ResourceStatus otherPropertyStatus = StatusUnknown; // valid values are Unknown/2xxOk/NotOk
        Q_FOREACH (const QVariant &vpropstat, propstats) {
            QVariantMap propstat = vpropstat.toMap();
            const QVariantMap &prop(propstat.value("prop").toMap());
            if (prop.contains("getctag")) {
                currInfo.ctag = prop.value("getctag").toMap().value("@text").toString();
            }
            if (prop.contains("sync-token")) {
                currInfo.syncToken = prop.value("sync-token").toMap().value("@text").toString();
            }
            if (prop.contains("displayname")) {
                currInfo.displayName = prop.value("displayname").toMap().value("@text").toString();
            }
            bool thisPropstatIsForResourceType = false;
            if (prop.contains("resourcetype")) {
                thisPropstatIsForResourceType = true;
                QStringList resourceTypeKeys = prop.value("resourcetype").toMap().keys();
                if ((resourceTypeKeys.size() == 1 && resourceTypeKeys.contains(QStringLiteral("collection"), Qt::CaseInsensitive))
                        || (resourceTypeKeys.contains(QStringLiteral("addressbook"), Qt::CaseInsensitive))) {
                    // This is probably a carddav addressbook collection.
                    // Despite section 5.2 of RFC6352 stating that a CardDAV
                    // server MUST return the 'addressbook' value in the resource types
                    // property, some CardDAV implementations (eg, Memotoo) do not.
                    addressbookResourceSpecified = StatusExplicitlyTrue;
                    LOG_DEBUG(Q_FUNC_INFO << "have addressbook resource:" << currInfo.url);
                } else {
                    // the resource is explicitly described as non-addressbook resource.
                    addressbookResourceSpecified = StatusExplicitlyFalse;
                    LOG_DEBUG(Q_FUNC_INFO << "have non-addressbook resource:" << currInfo.url);
                }
            }
            // Some services (e.g. Cozy) return multiple propstats
            // where only one will refer to the resourcetype property itself;
            // others will refer to incidental properties like displayname etc.
            // Each propstat will (should) contain a status code, which applies
            // only to the properties referred to within the propstat.
            // Thus, a 404 code may only apply to a displayname, etc.
            if (propstat.contains("status")) {
                static const QRegularExpression Http2xxOk("2[0-9][0-9]");
                QString status = propstat.value("status").toMap().value("@text").toString();
                bool statusOk = status.contains(Http2xxOk); // any HTTP 2xx OK response
                if (thisPropstatIsForResourceType) {
                    // This status applies to the resourcetype property.
                    if (statusOk) {
                        resourcetypeStatus = StatusExplicitly2xxOk; // explicitly ok
                    } else {
                        resourcetypeStatus = StatusExplicitlyNotOk; // explicitly not ok
                        LOG_DEBUG(Q_FUNC_INFO << "response has non-OK status:" << status
                                              << "for properties:" << prop.keys()
                                              << "for url:" << currInfo.url);
                    }
                } else {
                    // This status applies to some other property.
                    // In some cases (e.g. Memotoo) we may need
                    // to infer that this status refers to the
                    // entire response.
                    if (statusOk) {
                        otherPropertyStatus = StatusExplicitly2xxOk; // explicitly ok
                    } else {
                        otherPropertyStatus = StatusExplicitlyNotOk; // explicitly not ok
                        LOG_DEBUG(Q_FUNC_INFO << "response has non-OK status:" << status
                                              << "for non-resourcetype properties:" << prop.keys()
                                              << "for url:" << currInfo.url);
                    }
                }
            }
        }

        // now check to see if we have all of the required information
        if (resourcetypeStatus == StatusExplicitly2xxOk) {
            // we definitely had a well-specified resourcetype response, with 200 OK status.
            LOG_DEBUG(Q_FUNC_INFO << "have addressbook resource with status OK:" << currInfo.url);
        } else if (propstats.count() == 1                          // only one response element
                && addressbookResourceSpecified == StatusUnknown   // resource type unknown
                && otherPropertyStatus == StatusExplicitly2xxOk) { // status was explicitly ok
            // we assume that this was an implicit Addressbook Collection resourcetype response.
            LOG_DEBUG(Q_FUNC_INFO << "have probable addressbook resource with status OK:" << currInfo.url);
        } else {
            // we either cannot infer that this was an Addressbook Collection
            // or we were told explicitly that the collection status was NOT OK.
            LOG_DEBUG(Q_FUNC_INFO << "ignoring resource:" << currInfo.url << "due to type or status:"
                                  << addressbookResourceSpecified << resourcetypeStatus << otherPropertyStatus);
            continue;
        }

        // add the addressbook to our return list.  If we have no sync-token or c-tag, we do manual delta detection.
        if (currInfo.ctag.isEmpty() && currInfo.syncToken.isEmpty()) {
            LOG_DEBUG(Q_FUNC_INFO << "addressbook:" << currInfo.url << "has no sync-token or c-tag");
        } else {
            LOG_DEBUG(Q_FUNC_INFO << "found valid addressbook:" << currInfo.url << "with sync-token or c-tag");
        }
        infos.append(currInfo);
    }

    return infos;
}

QList<ReplyParser::ContactInformation> ReplyParser::parseSyncTokenDelta(const QByteArray &syncTokenDeltaResponse, QString *newSyncToken) const
{
    /* We expect a response of the form:
        <?xml version="1.0" encoding="utf-8" ?>
        <d:multistatus xmlns:d="DAV:">
            <d:response>
                <d:href>/addressbooks/johndoe/contacts/newcard.vcf</d:href>
                <d:propstat>
                    <d:prop>
                        <d:getetag>"33441-34321"</d:getetag>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
            <d:response>
                <d:href>/addressbooks/johndoe/contacts/updatedcard.vcf</d:href>
                <d:propstat>
                    <d:prop>
                        <d:getetag>"33541-34696"</d:getetag>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
            <d:response>
                <d:href>/addressbooks/johndoe/contacts/deletedcard.vcf</d:href>
                <d:status>HTTP/1.1 404 Not Found</d:status>
            </d:response>
            <d:sync-token>http://sabredav.org/ns/sync/5001</d:sync-token>
         </d:multistatus>
    */
    debugDumpData(QString::fromUtf8(syncTokenDeltaResponse));
    QList<ReplyParser::ContactInformation> info;
    QXmlStreamReader reader(syncTokenDeltaResponse);
    QVariantMap vmap = xmlToVMap(reader);
    QVariantMap multistatusMap = vmap[QLatin1String("multistatus")].toMap();
    if (newSyncToken) {
        *newSyncToken = multistatusMap.value("sync-token").toMap().value("@text").toString();
    }

    QVariantList responses;
    if (multistatusMap[QLatin1String("response")].type() == QVariant::List) {
        // multiple updates in the delta.
        responses = multistatusMap[QLatin1String("response")].toList();
    } else {
        // only one update in the delta.
        QVariantMap response = multistatusMap[QLatin1String("response")].toMap();
        responses << response;
    }

    Q_FOREACH (const QVariant &rv, responses) {
        QVariantMap rmap = rv.toMap();
        ReplyParser::ContactInformation currInfo;
        currInfo.uri = QUrl::fromPercentEncoding(rmap.value("href").toMap().value("@text").toString().toUtf8());
        currInfo.etag = rmap.value("propstat").toMap().value("prop").toMap().value("getetag").toMap().value("@text").toString();
        QMap<QString, QString>::const_iterator it = q->m_contactUris.constBegin();
        for ( ; it != q->m_contactUris.constEnd(); ++it) {
            if (it.value() == currInfo.uri) {
                currInfo.guid = it.key();
            }
        }
        QString status = rmap.value("propstat").toMap().value("status").toMap().value("@text").toString();
        if (status.contains(QLatin1String("200 OK"))) {
            if (!currInfo.uri.endsWith(QStringLiteral(".vcf"), Qt::CaseInsensitive)) {
                // this is probably a response for the addressbook resource,
                // rather than for a contact resource within the addressbook.
                LOG_DEBUG(Q_FUNC_INFO << "ignoring non-contact resource:" << currInfo.uri << currInfo.etag << status);
                continue;
            }
            currInfo.modType = currInfo.guid.isEmpty()
                             ? ReplyParser::ContactInformation::Addition
                             : ReplyParser::ContactInformation::Modification;
        } else if (status.contains(QLatin1String("404 Not Found"))) {
            currInfo.modType = ReplyParser::ContactInformation::Deletion;
        } else {
            LOG_WARNING(Q_FUNC_INFO << "unknown response:" << currInfo.uri << currInfo.etag << status);
        }
        info.append(currInfo);
    }

    return info;
}

QList<ReplyParser::ContactInformation> ReplyParser::parseContactMetadata(const QByteArray &contactMetadataResponse, const QString &addressbookUrl) const
{
    /* We expect a response of the form:
        HTTP/1.1 207 Multi-status
        Content-Type: application/xml; charset=utf-8

        <d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
            <d:response>
                <d:href>/addressbooks/johndoe/contacts/abc-def-fez-123454657.vcf</d:href>
                <d:propstat>
                    <d:prop>
                        <d:getetag>"2134-888"</d:getetag>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
            <d:response>
                <d:href>/addressbooks/johndoe/contacts/acme-12345.vcf</d:href>
                <d:propstat>
                    <d:prop>
                        <d:getetag>"9999-2344""</d:getetag>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
        </d:multistatus>
    */
    debugDumpData(QString::fromUtf8(contactMetadataResponse));
    QList<ReplyParser::ContactInformation> info;
    QXmlStreamReader reader(contactMetadataResponse);
    QVariantMap vmap = xmlToVMap(reader);
    QVariantMap multistatusMap = vmap[QLatin1String("multistatus")].toMap();
    QVariantList responses;
    if (multistatusMap[QLatin1String("response")].type() == QVariant::List) {
        // multiple updates in the delta.
        responses = multistatusMap[QLatin1String("response")].toList();
    } else {
        // only one update in the delta.
        QVariantMap response = multistatusMap[QLatin1String("response")].toMap();
        responses << response;
    }

    QSet<QString> seenUris;
    Q_FOREACH (const QVariant &rv, responses) {
        QVariantMap rmap = rv.toMap();
        ReplyParser::ContactInformation currInfo;
        currInfo.uri = QUrl::fromPercentEncoding(rmap.value("href").toMap().value("@text").toString().toUtf8());
        currInfo.etag = rmap.value("propstat").toMap().value("prop").toMap().value("getetag").toMap().value("@text").toString();
        QString status = rmap.value("propstat").toMap().value("status").toMap().value("@text").toString();
        if (!currInfo.uri.endsWith(QStringLiteral(".vcf"), Qt::CaseInsensitive)) {
            // this is probably a response for the addressbook resource,
            // rather than for a contact resource within the addressbook.
            LOG_DEBUG(Q_FUNC_INFO << "ignoring non-contact resource:" << currInfo.uri << currInfo.etag << status);
            continue;
        }
        QMap<QString, QString>::const_iterator it = q->m_contactUris.constBegin();
        for ( ; it != q->m_contactUris.constEnd(); ++it) {
            if (it.value() == currInfo.uri) {
                currInfo.guid = it.key();
            }
        }
        if (status.contains(QLatin1String("200 OK"))) {
            seenUris.insert(currInfo.uri);
            currInfo.modType = currInfo.guid.isEmpty()
                             ? ReplyParser::ContactInformation::Addition
                             : ReplyParser::ContactInformation::Modification;
            // only append if it's an addition or an actual modification
            // the etag will have changed since the last time we saw it,
            // if the contact has been modified server-side since last sync.
            if (currInfo.modType == ReplyParser::ContactInformation::Addition) {
                LOG_TRACE("Resource" << currInfo.uri << "was added on server with etag" << currInfo.etag);
                info.append(currInfo);
            } else if (q->m_contactEtags[currInfo.guid] != currInfo.etag) {
                LOG_TRACE("Resource" << currInfo.uri << "with guid" << currInfo.guid << "was modified on server.");
                LOG_TRACE("Old etag:" << q->m_contactEtags[currInfo.guid] << "New etag:" << currInfo.etag);
                info.append(currInfo);
            } else {
                LOG_TRACE("Resource" << currInfo.uri << "with guid" << currInfo.guid << "is unchanged since last sync with etag" << currInfo.etag);
            }
        } else {
            LOG_WARNING(Q_FUNC_INFO << "unknown response:" << currInfo.uri << currInfo.etag << status);
        }
    }

    // we now need to determine deletions.
    QStringList contactGuidsInAddressbook = q->m_addressbookContactGuids[addressbookUrl];
    Q_FOREACH (const QString &guid, contactGuidsInAddressbook) {
        const QString &uri(q->m_contactUris[guid]);
        if (!seenUris.contains(uri)) {
            // this uri wasn't listed in the report, so this contact must have been deleted.
            LOG_TRACE("Resource" << uri << "with guid" << guid << "was deleted on server");
            ReplyParser::ContactInformation currInfo;
            currInfo.etag = q->m_contactEtags[guid];
            currInfo.uri = uri;
            currInfo.guid = guid;
            currInfo.modType = ReplyParser::ContactInformation::Deletion;
            info.append(currInfo);
        }
    }

    return info;
}

QMap<QString, ReplyParser::FullContactInformation> ReplyParser::parseContactData(const QByteArray &contactData) const
{
    /* We expect a response of the form:
        HTTP/1.1 207 Multi-status
        Content-Type: application/xml; charset=utf-8

        <d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
            <d:response>
                <d:href>/addressbooks/johndoe/contacts/abc-def-fez-123454657.vcf</d:href>
                <d:propstat>
                    <d:prop>
                        <d:getetag>"2134-314"</d:getetag>
                        <card:address-data>BEGIN:VCARD
                            VERSION:3.0
                            FN:My Mother
                            UID:abc-def-fez-1234546578
                            END:VCARD
                        </card:address-data>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
            <d:response>
                <d:href>/addressbooks/johndoe/contacts/someapplication-12345678.vcf</d:href>
                <d:propstat>
                    <d:prop>
                        <d:getetag>"5467-323"</d:getetag>
                        <card:address-data>BEGIN:VCARD
                            VERSION:3.0
                            FN:Your Mother
                            UID:foo-bar-zim-gir-1234567
                            END:VCARD
                        </card:address-data>
                    </d:prop>
                    <d:status>HTTP/1.1 200 OK</d:status>
                </d:propstat>
            </d:response>
        </d:multistatus>
    */
    debugDumpData(QString::fromUtf8(contactData));
    QXmlStreamReader reader(contactData);
    QVariantMap vmap = xmlToVMap(reader);
    QVariantMap multistatusMap = vmap[QLatin1String("multistatus")].toMap();
    QVariantList responses;
    if (multistatusMap[QLatin1String("response")].type() == QVariant::List) {
        // multiple updates in the delta.
        responses = multistatusMap[QLatin1String("response")].toList();
    } else {
        // only one update in the delta.
        QVariantMap response = multistatusMap[QLatin1String("response")].toMap();
        responses << response;
    }

    QMap<QString, ReplyParser::FullContactInformation> uriToContactData;
    Q_FOREACH (const QVariant &rv, responses) {
        QVariantMap rmap = rv.toMap();
        QString uri = QUrl::fromPercentEncoding(rmap.value("href").toMap().value("@text").toString().toUtf8());
        QString etag = rmap.value("propstat").toMap().value("prop").toMap().value("getetag").toMap().value("@text").toString();
        QString vcard = rmap.value("propstat").toMap().value("prop").toMap().value("address-data").toMap().value("@text").toString();

        // import the data as a vCard
        bool ok = true;
        QPair<QContact, QStringList> result = m_converter->convertVCardToContact(vcard, &ok);
        if (!ok) {
            continue;
        }

        // fix up various details of the contact.
        QContact importedContact = result.first;
        QContactGuid guid = importedContact.detail<QContactGuid>();
        QString uid = guid.guid(); // at this stage it's a UID.
        if (uid.isEmpty()) {
            LOG_WARNING(Q_FUNC_INFO << "contact import from vcard has no UID:\n" << vcard);
            continue;
        }
        bool found = false;
        QMap<QString,QString>::const_iterator it = q->m_contactUids.constBegin();
        for ( ; it != q->m_contactUids.constEnd(); ++it) {
            // see if the UID exists in our map already
            if (it.value() == uid) {
                // it does; use the local-device GUID instead.
                guid.setGuid(it.key());
                found = true;
                break;
            }
        }
        if (!found) {
            // this is a server addition.  mutate the uid into a per-account device guid.
            guid.setGuid(QStringLiteral("%1:%2").arg(q->m_accountId).arg(uid));
            // also set the guid to uid mapping for the server-side addition.
            q->m_contactUids.insert(guid.guid(), uid);
        }
        importedContact.saveDetail(&guid);

        // and insert into the return map.
        ReplyParser::FullContactInformation fci;
        fci.contact = importedContact;
        fci.unsupportedProperties = result.second;
        fci.etag = etag;
        uriToContactData.insert(uri, fci);
    }

    return uriToContactData;
}

