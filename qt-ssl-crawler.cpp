/*************************************************************************************
**
** QtSslCrawl
** Copyright (C) 2012 Peter Hartmann <9qgm-76ea@xemaps.com>
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License as published by the Free Software Foundation; either
** version 2.1 of the License, or (at your option) any later version.
**
** This library is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public
** License along with this library; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
*************************************************************************************/

#include "qt-ssl-crawler.h"
#include <QFile>
#include <QUrl>
#include <QDebug>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QCoreApplication>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

// in reality the number of open connections is higher than the value below
int QtSslCrawler::s_concurrentRequests = 100;
QNetworkRequest::Attribute QtSslCrawler::s_tryCountAttribute =
        static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 1);

QtSslCrawler::QtSslCrawler(QObject *parent, int from, int to) :
    QObject(parent),
    m_manager(new QNetworkAccessManager(this)),
    m_crawlFrom(from),
    m_crawlTo(to)
{
    QFile domainFile(QStringLiteral("top-1m.csv"));
    if (!domainFile.open(QIODevice::ReadOnly)) {
        qFatal("could not open file 'top-1m.csv', download it from http://s3.amazonaws.com/alexa-static/top-1m.csv.zip");
    }
    int currentLine = 0;
    while (!domainFile.atEnd()) {
        currentLine++;
        QByteArray line = domainFile.readLine();
        if (m_crawlFrom == 0 || m_crawlTo == 0 || (m_crawlFrom <= currentLine && currentLine <= m_crawlTo)) {
            QByteArray domain = line.right(line.count() - line.indexOf(',') - 1).prepend("https://www.");
            QUrl url = QUrl::fromEncoded(domain.trimmed());
            QNetworkRequest request(url);
            // setting the attribute to trace the originating URL,
            // because we might try different URLs or get redirects
            request.setAttribute(QNetworkRequest::User, url);
            queueRequestIfNew(request); // all requests should be new here
            if (currentLine == m_crawlTo)
                break; // no need to crawl the rest of the file
        }
    }
}

void QtSslCrawler::start() {
    QMetaObject::invokeMethod(this, "checkForSendingMoreRequests", Qt::QueuedConnection);
}

void QtSslCrawler::foundUrl(const QUrl &foundUrl, const QUrl &originalUrl) {

    QNetworkRequest request(foundUrl);
    request.setAttribute(QNetworkRequest::User, originalUrl);
    queueRequestIfNew(request);
    QMetaObject::invokeMethod(this, "checkForSendingMoreRequests", Qt::QueuedConnection);
}

void QtSslCrawler::timeout() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender()->parent());
    finishRequest(reply);
    QNetworkRequest request = reply->request();
    int tryCount = request.attribute(QtSslCrawler::s_tryCountAttribute).toInt();
    if (tryCount == 0) {
        qDebug() << "timeout, re-scheduling request for" << request.url();
        tryCount++;
        QNetworkRequest newRequest(request);
        newRequest.setAttribute(QtSslCrawler::s_tryCountAttribute, QVariant(tryCount));
        m_visitedUrls.remove(reply->request().url()); // hack (has just been inserted by finishRequest)
        queueRequestIfNew(newRequest);
    } else {
        qDebug() << "timeout, tried" << request.url() << "twice, giving up.";
    }
    // we called checkForSendingMoreRequests() implicitly with finishRequest()
}

void QtSslCrawler::checkForSendingMoreRequests() {

    while (m_urlsWaitForFinished.count() < s_concurrentRequests
           && m_requestsToSend.count() > 0) {
        QNetworkRequest request = m_requestsToSend.dequeue();
        sendRequest(request);
    }
}

void QtSslCrawler::queueRequestIfNew(const QNetworkRequest &request) {

    if (!m_visitedUrls.contains(request.url())
        && !m_urlsWaitForFinished.contains(request.url())
        && !m_requestsToSend.contains(request)) {
        m_requestsToSend.enqueue(request);
    } else {
        qDebug() << "visited" << request.url() << "already or visiting it currently";
    }
}

void QtSslCrawler::sendRequest(const QNetworkRequest &request) {

    qDebug() << "sending request for" << request.url();
    QNetworkRequest newRequest(request);
    // do not keep connections open, we will not issue
    // more than one request to the same host
    newRequest.setRawHeader("Connection", "close");
    QNetworkReply *reply = m_manager->get(newRequest);
    // if there is neither error nor success after 5 minutes,
    // try again one more time and then skip the URL.
    // (The timer will be destroyed if the reply finished after 5 minutes)
    QTimer *timer = new QTimer(reply);
    connect(timer, SIGNAL(timeout()), this, SLOT(timeout()));
    timer->setSingleShot(true);
    timer->start(300000); // 5 minutes

    reply->ignoreSslErrors(); // we don't care, we just want the certificate
    connect(reply, SIGNAL(metaDataChanged()), this, SLOT(replyMetaDataChanged()));
    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
            this, SLOT(replyError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(replyFinished()));

    m_urlsWaitForFinished.insert(request.url());
}

void QtSslCrawler::finishRequest(QNetworkReply *reply) {

    reply->disconnect(SIGNAL(metaDataChanged()));
    reply->disconnect(SIGNAL(error(QNetworkReply::NetworkError)));
    reply->disconnect(SIGNAL(finished()));
    reply->close();
    reply->abort();
    reply->deleteLater();
    m_visitedUrls.insert(reply->request().url());
    m_urlsWaitForFinished.remove(reply->request().url());
    qDebug() << "finishRequest pending requests:" << m_requestsToSend.count() + m_urlsWaitForFinished.count();
    QMetaObject::invokeMethod(this, "checkForSendingMoreRequests", Qt::QueuedConnection);
    if (m_urlsWaitForFinished.count() + m_requestsToSend.count() == 0) {
        emit crawlFinished();
    }
}

void QtSslCrawler::replyMetaDataChanged() {

    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QUrl currentUrl = reply->url();
    QUrl originalUrl = reply->request().attribute(QNetworkRequest::User).toUrl();

    qDebug() << "replyMetaDataChanged" << currentUrl << "original url:" << originalUrl;

    if (reply->error() == QNetworkReply::NoError) {
        if (currentUrl.scheme() == QLatin1String("https")) {
            // success, https://[domain] exists and serves meaningful content
            QList<QSslCertificate> chain = reply->sslConfiguration().peerCertificateChain();
            if (!chain.empty()) {
                QStringList organizations = chain.last().issuerInfo(QSslCertificate::Organization);
                emit crawlResult(originalUrl, currentUrl, chain);
                qDebug() << "found ssl cert at" << currentUrl
                        << "organizations:" << organizations << ", coming from" << originalUrl;
            } else {
                // never saw that happen
                qWarning() << "weird: no errors but certificate chain is empty for " << reply->url();
            }

        } else if (currentUrl.scheme() == QLatin1String("http")) {
            // check for redirections, we might end up at an SSL site
            int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (statusCode >= 300 && statusCode < 400) {
                QByteArray locationHeader = reply->header(QNetworkRequest::LocationHeader).toByteArray();
                if (locationHeader.isEmpty()) // this seems to be a bug in QtNetwork
                    locationHeader = reply->rawHeader("Location");
                QUrl newUrl = QUrl::fromEncoded(locationHeader);
                if (!newUrl.isEmpty()) {
                    qDebug() << "found redirect header at" << currentUrl << "to" << newUrl;
                    QNetworkRequest request(newUrl);
                    request.setAttribute(QNetworkRequest::User, originalUrl);
                    queueRequestIfNew(request);
                    QMetaObject::invokeMethod(this, "checkForSendingMoreRequests", Qt::QueuedConnection);
                }
            } else {
                qDebug() << "meta data changed for" << currentUrl << "do nothing I guess, wait for finished";
            }
        } else {
            // never saw that happen
            qWarning() << "scheme for" << currentUrl << "is neither https nor http";
        }

    } else { // there was an error

        // does not happen
        qDebug() << "error with" << currentUrl << reply->errorString();
    }
}

void QtSslCrawler::replyError(QNetworkReply::NetworkError error) {

    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QUrl currentUrl = reply->url();
    QUrl originalUrl = reply->request().attribute(QNetworkRequest::User).toUrl();

    qDebug() << "replyError" << error << currentUrl << reply->errorString() << "original url:" << originalUrl;
    // 2nd try: if https://[domain] does not work, fetch
    // http://[domain] and parse the HTML for https:// URLs

    // ### check which error we got

    // our blind check for https://[domain] was not succesful, try http://[domain] now
    if (originalUrl.host() == currentUrl.host() && currentUrl.scheme() == QLatin1String("https")) {
        QUrl newUrl = currentUrl;
        newUrl.setScheme(QStringLiteral("http"));
        QNetworkRequest newRequest(newUrl); // ### probably we can just copy it
        newRequest.setAttribute(QNetworkRequest::User, newUrl);
        qDebug() << "queueing new request" << newUrl << "original url:" << newUrl;
        queueRequestIfNew(newRequest);
    } else {
        qWarning() << "could not fetch" << currentUrl << "original url:" << originalUrl; // ### try again?
    }
    finishRequest(reply);
}

void QtSslCrawler::replyFinished() {

    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QUrl currentUrl = reply->url();
    QUrl originalUrl = reply->request().attribute(QNetworkRequest::User).toUrl();
    if (reply->error() == QNetworkReply::NoError) {
        qDebug() << "reply finished:" << currentUrl << "original url:" << originalUrl << ", now grep for urls";
        QByteArray replyData = reply->readAll();
        // now start the job to find URLs in a new thread
        UrlFinderRunnable *runnable = new UrlFinderRunnable(replyData, originalUrl, currentUrl);
        connect(runnable, SIGNAL(foundUrl(QUrl,QUrl)), this, SLOT(foundUrl(QUrl,QUrl)), Qt::QueuedConnection);
        QThreadPool::globalInstance()->start(runnable);
    } else {
        qWarning() << "got error while parsing" << currentUrl << "for" << originalUrl << reply->errorString();
    }
    finishRequest(reply);
}

UrlFinderRunnable::UrlFinderRunnable(const QByteArray &data, const QUrl &originalUrl, const QUrl &currentUrl) :
        QObject(), QRunnable(), m_data(data), m_originalUrl(originalUrl), m_currentUrl(currentUrl),
        m_regExp("(https://[a-z0-9.@:]+)", Qt::CaseInsensitive) {
}

void UrlFinderRunnable::run() {

    int pos = 0;
    while ((pos = m_regExp.indexIn(m_data, pos)) != -1) {
        QUrl newUrl(m_regExp.cap(1));
        if (newUrl.isValid()
            && newUrl.host().contains('.') // filter out 'https://ssl'
            && newUrl.host() != QLatin1String("ssl.")
            && newUrl.host() != m_originalUrl.host()
            && newUrl != m_currentUrl) { // prevent endless loops
            qDebug() << "runnable: found valid url" << newUrl << "at original url" << m_originalUrl;
            emit foundUrl(newUrl, m_originalUrl);
        }
        pos += m_regExp.matchedLength();
    }
}
