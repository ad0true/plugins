/*
 * psi-otr.cpp - off-the-record messaging plugin for psi
 *
 * Copyright (C) Timo Engel (timo-e@freenet.de), Berlin 2007.
 * This program was written as part of a diplom thesis advised by 
 * Prof. Dr. Ruediger Weis (PST Labor)
 * at the Technical University of Applied Sciences Berlin.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "PsiOtrPlugin.hpp"
#include "psiotrclosure.h"
#include "PsiOtrConfig.hpp"
#include "applicationinfoaccessinghost.h"
#include "HtmlTidy.hpp"

namespace psiotr
{

// ---------------------------------------------------------------------------

namespace
{

// ---------------------------------------------------------------------------

/**
 * Removes the resource from a given JID. 
 * Example:
 * removeResource("user@jabber.org/Home")
 * returns "user@jabber.org"
 */
QString removeResource(const QString& aJid)
{
    QString addr(aJid);
    int pos = aJid.indexOf("/");
    if (pos > -1)
    {
        addr.truncate(pos);
        return addr;
    }
    return addr;
}

// ---------------------------------------------------------------------------

/**
 * Reverts Qt::escape()
 */
QString unescape(const QString& escaped)
{
    QString plain = escaped;
    plain.replace("&lt;", "<")
         .replace("&gt;", ">")
         .replace("&quot;", "\"")
         .replace("&amp;", "&");
    return plain;
}

// ---------------------------------------------------------------------------

} // namespace

// ===========================================================================

PsiOtrPlugin::PsiOtrPlugin()
    : m_enabled(false),
      m_otrConnection(NULL),
      m_onlineUsers(),
      m_psiDataDir(),
      m_optionHost(NULL),
      m_senderHost(NULL),
      m_applicationInfo(NULL),
      m_accountInfo(NULL),
      m_contactInfo(NULL)
{
}

// ---------------------------------------------------------------------------

PsiOtrPlugin::~PsiOtrPlugin()
{
}

// ---------------------------------------------------------------------------

QString PsiOtrPlugin::name() const
{
    return "Off-the-Record Messaging";
}

// ---------------------------------------------------------------------------

QString PsiOtrPlugin::shortName() const
{
    return "otr";
}

// ---------------------------------------------------------------------------

QString PsiOtrPlugin::version() const
{
    return "0.9";
}

// ---------------------------------------------------------------------------

QWidget* PsiOtrPlugin::options()
{
    if (!m_enabled)
    {
        return 0;
    }
    else
    {
        return new ConfigDialog(m_otrConnection, m_optionHost);
    }
} 

// ---------------------------------------------------------------------------

bool PsiOtrPlugin::enable()
{
    QVariant policyOption = m_optionHost->getPluginOption(PSI_CONFIG_POLICY);
    m_otrConnection = new OtrMessaging(this,
                                       static_cast<OtrPolicy>(policyOption.toInt()));
    m_enabled = true;
    return true;
}

// ---------------------------------------------------------------------------

bool PsiOtrPlugin::disable()
{
    foreach (QString account, m_onlineUsers.keys())
    {
        foreach(QString jid, m_onlineUsers.value(account).keys())
        {
            m_otrConnection->endSession(account, jid);
            m_onlineUsers[account][jid]->updateMessageState();
            m_onlineUsers[account][jid]->disable();
        }
    }

    
    delete m_otrConnection;
    m_enabled = false;
    return true;
}

// ---------------------------------------------------------------------------

void PsiOtrPlugin::applyOptions()
{
}

// ---------------------------------------------------------------------------

void PsiOtrPlugin::restoreOptions()
{
}

//-----------------------------------------------------------------------------

bool PsiOtrPlugin::processEvent(int accountNo, QDomElement& e)
{
    if (m_enabled && e.attribute("type") == "MessageEvent" &&
        !e.firstChildElement("message").isNull())
    {
        QDomElement messageElement = e.firstChildElement("message");

        QString contact = getCorrectJid(accountNo,
                                        messageElement.attribute("from"));
        QString account = m_accountInfo->getId(accountNo);

        QDomElement htmlElement = messageElement.firstChildElement("html");
        QDomElement plainBody   = messageElement.firstChildElement("body");
        QString cyphertext;
        if (!htmlElement.isNull())
        {
            QTextStream textStream(&cyphertext);
            htmlElement.firstChildElement("body").save(textStream, 0);
        }
        else if (!plainBody.isNull())
        {
            cyphertext = plainBody.firstChild().toText().nodeValue();
        }
        else
        {
            return false;
        }

        QString decrypted;
        if (m_otrConnection->decryptMessage(contact, account, cyphertext,
                                            decrypted))
        {
            if (m_onlineUsers.contains(account) && 
                m_onlineUsers.value(account).contains(contact))
            {
                m_onlineUsers[account][contact]->updateMessageState();
            }

            QString bodyText;

            bool isHTML = !htmlElement.isNull() ||
                          Qt::mightBeRichText(decrypted);

            if (!isHTML)
            {
                bodyText = decrypted;
            }
            else
            {
                HtmlTidy htmlTidy("<body xmlns=\"http://www.w3.org/1999/xhtml\">" +
                                  decrypted + "</body>");
                decrypted = htmlTidy.output();

                bodyText = decrypted;
                bodyText.replace("\n", " ")
                        .replace(QRegExp("<br(?:\\s[^>]*)?/>"), "\n")
                        .replace(QRegExp("<b(?:\\s[^>]*)?>([^<]+)</b>"), "*\\1*")
                        .replace(QRegExp("<i(?:\\s[^>]*)?>([^<]+)</i>"), "/\\1/")
                        .replace(QRegExp("<u(?:\\s[^>]*)?>([^<]+)</u>"), "_\\1_")
                        .remove(QRegExp("<[^>]*>"));

                // replace html body
                if (htmlElement.isNull())
                {
                    htmlElement = e.ownerDocument().createElement("html");
                    htmlElement.setAttribute("xmlns",
                                             "http://jabber.org/protocol/xhtml-im");
                    messageElement.appendChild(htmlElement);
                }
                else
                {
                    htmlElement.removeChild(htmlElement.firstChildElement("body"));
                }

                QDomDocument document;
                int errorLine = 0, errorColumn = 0;
                QString errorText;
                if (document.setContent(decrypted, true, &errorText, &errorLine,
                                        &errorColumn))
                {
                    htmlElement.appendChild(document.documentElement());
                }
                else
                {
                    qWarning() << "---- parsing error:\n" << decrypted <<
                                  "\n----\n" << errorText << " line:" <<
                                  errorLine << " column:" << errorColumn;
                    messageElement.removeChild(htmlElement);
                }
            }

            // replace plaintext body
            plainBody.removeChild(plainBody.firstChild());
            plainBody.appendChild(e.ownerDocument().createTextNode(unescape(bodyText)));
        }
    }
    return false;
}

//-----------------------------------------------------------------------------

bool PsiOtrPlugin::processMessage(int, const QString&, const QString&,
                                  const QString&)
{
    return false;
}

//-----------------------------------------------------------------------------

bool PsiOtrPlugin::processOutgoingMessage(int accountNo, const QString& toJid,
                                          QString& body, const QString& type,
                                          QString&)
{
    if (!m_enabled || type == "groupchat")
    {
        return false;
    }

    QString account = m_accountInfo->getId(accountNo);

    QString encrypted = m_otrConnection->encryptMessage(
        account,
        getCorrectJid(accountNo, toJid),
        Qt::escape(body));

    body = unescape(encrypted);

    return false;
}

// ---------------------------------------------------------------------------

void PsiOtrPlugin::logout(int accountNo)
{
    if (!m_enabled)
    {
        return;
    }
    
    QString account = m_accountInfo->getId(accountNo);

    if (m_onlineUsers.contains(account))
    {
        foreach(QString jid, m_onlineUsers.value(account).keys())
        {
            m_otrConnection->endSession(account, jid);
            m_onlineUsers[account][jid]->setIsLoggedIn(false);
            m_onlineUsers[account][jid]->updateMessageState();
        }
    }
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::setOptionAccessingHost(OptionAccessingHost* host)
{
    m_optionHost = host;
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::optionChanged(const QString&)
{
    QVariant policyOption = m_optionHost->getPluginOption(PSI_CONFIG_POLICY);
    m_otrConnection->setPolicy(static_cast<OtrPolicy>(policyOption.toInt()));
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::setStanzaSendingHost(StanzaSendingHost *host)
{
    m_senderHost = host;
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::setApplicationInfoAccessingHost(ApplicationInfoAccessingHost* host)
{
    m_applicationInfo = host;
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::setAccountInfoAccessingHost(AccountInfoAccessingHost *host)
{
    m_accountInfo = host;
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::setContactInfoAccessingHost(ContactInfoAccessingHost *host)
{
    m_contactInfo = host;
}

//-----------------------------------------------------------------------------

bool PsiOtrPlugin::incomingStanza(int accountNo, const QDomElement& xml)
{
    if (!m_enabled || xml.nodeName() != "presence")
    {
        return false;
    }
    
    QString account = m_accountInfo->getId(accountNo);
    QString contact = getCorrectJid(accountNo, xml.attribute("from"));
    QString type = xml.attribute("type", "available");
    
    if (type == "available")
    {
        if (!m_onlineUsers.value(account).contains(contact))
        {
            m_onlineUsers[account][contact] = new PsiOtrClosure(account,
                                                                contact,
                                                                m_otrConnection);
        }
        
        m_onlineUsers[account][contact]->setIsLoggedIn(true);
    }
    else if (type == "unavailable")
    {
        if (m_onlineUsers.contains(account) && 
            m_onlineUsers.value(account).contains(contact))
        {
            m_onlineUsers[account][contact]->setIsLoggedIn(false);
            m_onlineUsers[account][contact]->updateMessageState();
        }
    }

    return false;
}

//-----------------------------------------------------------------------------

bool PsiOtrPlugin::outgoingStanza(int accountNo, QDomElement& xml)
{
    if (!m_enabled || xml.nodeName() != "message")
    {
        return false;
    }

    QString account = m_accountInfo->getId(accountNo);
    QString contact = getCorrectJid(accountNo, xml.attribute("to"));

    if (!m_onlineUsers.value(account).contains(contact))
    {
        m_onlineUsers[account][contact] = new PsiOtrClosure(account, contact,
                                                            m_otrConnection);
    }

    QDomElement htmlElement = xml.firstChildElement("html");
    if (m_onlineUsers[account][contact]->encrypted() && !htmlElement.isNull())
    {
        xml.removeChild(htmlElement);
    }

    return false;
}

//-----------------------------------------------------------------------------

QList<QVariantHash> PsiOtrPlugin::getButtonParam()
{
    return QList<QVariantHash>();
}

//-----------------------------------------------------------------------------

QAction* PsiOtrPlugin::getAction(QObject* parent, int accountNo,
                                 const QString& contactJid)
{
    if (!m_enabled)
    {
        return 0;
    }

    QString contact = getCorrectJid(accountNo, contactJid);
    QString account = m_accountInfo->getId(accountNo);
    
    if (!m_onlineUsers.value(account).contains(contact))
    {
        m_onlineUsers[account][contact] = new PsiOtrClosure(account,
                                                            contact,
                                                            m_otrConnection);
    }

    return m_onlineUsers[account][contact]->getChatDlgMenu(parent);
}

//-----------------------------------------------------------------------------

QString PsiOtrPlugin::dataDir()
{
    return m_applicationInfo->appCurrentProfileDir(
    ApplicationInfoAccessingHost::DataLocation);
}
    
//-----------------------------------------------------------------------------

void PsiOtrPlugin::sendMessage(const QString& account, const QString& toJid,
                               const QString& message)
{
    int accountIndex = getAccountIndexById(account);
    if (accountIndex != -1)
    {
        m_senderHost->sendMessage(accountIndex, toJid, message, "", "chat");
    }
}

//-----------------------------------------------------------------------------

bool PsiOtrPlugin::isLoggedIn(const QString& account, const QString& jid)
{
    if (m_onlineUsers.contains(account) &&
        m_onlineUsers.value(account).contains(jid))
    {
        return m_onlineUsers.value(account).value(jid)->isLoggedIn();
    }

    return false;
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::notifyUser(const OtrNotifyType& type, const QString& message)
{
    QMessageBox::Icon messageBoxIcon;
    if (type == OTR_NOTIFY_ERROR)
    {
        messageBoxIcon = QMessageBox::Critical; 
    }
    else if (type == OTR_NOTIFY_WARNING)
    {
        messageBoxIcon = QMessageBox::Warning;
    }
    else
    {
        messageBoxIcon = QMessageBox::Information;
    }

    QMessageBox mb(messageBoxIcon, tr("Psi OTR"), message, QMessageBox::Ok, NULL,
                   Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    mb.exec();
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::stopMessages()
{
    m_enabled = false;
}

//-----------------------------------------------------------------------------

void PsiOtrPlugin::startMessages()
{
    m_enabled = true;
}

//-----------------------------------------------------------------------------

QString PsiOtrPlugin::humanAccount(const QString accountId)
{
    return getAccountNameById(accountId);
}

// ---------------------------------------------------------------------------

int PsiOtrPlugin::getAccountIndexById(const QString& accountId)
{
    QString id;
    int accountIndex = 0;
    while (((id = m_accountInfo->getId(accountIndex)) != "-1") &&
           (id != accountId))
    {
        accountIndex++;
    }
    return (id == "-1")? -1 : accountIndex;
}

// ---------------------------------------------------------------------------

QString PsiOtrPlugin::getAccountNameById(const QString& accountId)
{
    return m_accountInfo->getName(getAccountIndexById(accountId));
}

// ---------------------------------------------------------------------------

QString PsiOtrPlugin::getAccountJidById(const QString& accountId)
{
    return m_accountInfo->getJid(getAccountIndexById(accountId));
}

// ---------------------------------------------------------------------------

QString PsiOtrPlugin::getCorrectJid(int account, const QString& fullJid)
{
    QString correctJid;
    if (m_contactInfo->isPrivate(account, fullJid))
    {
        correctJid = fullJid;
    }
    else
    {
        correctJid = removeResource(fullJid);

        // If the contact is private but not (yet) in the roster,
        // it will not be known as private.
        // Therefore, check if the bare Jid is a conference.
        if (m_contactInfo->isConference(account, correctJid)) {
            correctJid = fullJid;
        }
    }
    return correctJid;
}

//-----------------------------------------------------------------------------

} // namespace psiotr

//-----------------------------------------------------------------------------

Q_EXPORT_PLUGIN2(psiOtrPlugin, psiotr::PsiOtrPlugin)

//-----------------------------------------------------------------------------
