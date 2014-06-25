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

#ifndef REPLYPARSER_P_H
#define REPLYPARSER_P_H

#include <QObject>
#include <QString>
#include <QList>
#include <QByteArray>

#include <QContact>

QTCONTACTS_USE_NAMESPACE

class CardDavVCardConverter;
class Syncer;
class ReplyParser
{
public:
    class AddressBookInformation {
        public:
        QString url;
        QString displayName;
        QString ctag;
        QString syncToken;
    };

    class ContactInformation {
        public:
        enum ModificationType {
            Uninitialized = 0,
            Addition,
            Modification,
            Deletion
        };
        ContactInformation() : modType(Uninitialized) {}
        ModificationType modType;
        QString uri;
        QString guid; // this is the prefixed form of the UID (accountNumber:UID)
        QString etag;
    };

    class FullContactInformation {
        public:
        QContact contact;
        QStringList unsupportedProperties;
        QString etag;
    };

    ReplyParser(Syncer *parent, CardDavVCardConverter *converter);
    ~ReplyParser();

    QString parseUserPrinciple(const QByteArray &userInformationResponse) const;
    QString parseAddressbookHome(const QByteArray &addressbookUrlsResponse) const;
    QList<AddressBookInformation> parseAddressbookInformation(const QByteArray &addressbookInformationResponse) const;
    QList<ContactInformation> parseSyncTokenDelta(const QByteArray &syncTokenDeltaResponse, QString *newSyncToken) const;
    QList<ContactInformation> parseContactMetadata(const QByteArray &contactMetadataResponse, const QString &addresbookUrl) const;
    QMap<QString, FullContactInformation> parseContactData(const QByteArray &contactData) const;

private:
    Syncer *q;
    mutable CardDavVCardConverter *m_converter;
};

#endif // REPLYPARSER_P_H

