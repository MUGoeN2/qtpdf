/******************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the Qt PDF Module.
**
** $QT_BEGIN_LICENSE:COMM$
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** $QT_END_LICENSE$
**
******************************************************************************/

#include "qpdfdocument.h"
#include "qpdfdocument_p.h"

#include "public/fpdf_doc.h"

#include <QFile>
#include <QMutex>

QT_BEGIN_NAMESPACE

// The library is not thread-safe at all, it has a lot of global variables.
Q_GLOBAL_STATIC_WITH_ARGS(QMutex, pdfMutex, (QMutex::Recursive));
static int libraryRefCount;

QPdfMutexLocker::QPdfMutexLocker()
    : QMutexLocker(pdfMutex())
{
}

QPdfDocumentPrivate::QPdfDocumentPrivate()
    : avail(Q_NULLPTR)
    , doc(Q_NULLPTR)
    , loadComplete(false)
    , status(QPdfDocument::Null)
    , lastError(QPdfDocument::NoError)
{
    asyncBuffer.setData(QByteArray());
    asyncBuffer.open(QIODevice::ReadWrite);

    const QPdfMutexLocker lock;

    if (libraryRefCount == 0)
        FPDF_InitLibrary();
    ++libraryRefCount;

    // FPDF_FILEACCESS setup
    m_Param = this;
    m_GetBlock = fpdf_GetBlock;

    // FX_FILEAVAIL setup
    FX_FILEAVAIL::version = 1;
    IsDataAvail = fpdf_IsDataAvail;

    // FX_DOWNLOADHINTS setup
    FX_DOWNLOADHINTS::version = 1;
    AddSegment = fpdf_AddSegment;
}

QPdfDocumentPrivate::~QPdfDocumentPrivate()
{
    q->close();

    const QPdfMutexLocker lock;

    if (!--libraryRefCount)
        FPDF_DestroyLibrary();
}

void QPdfDocumentPrivate::clear()
{
    QPdfMutexLocker lock;

    if (doc)
        FPDF_CloseDocument(doc);
    doc = Q_NULLPTR;

    if (avail)
        FPDFAvail_Destroy(avail);
    avail = Q_NULLPTR;
    lock.unlock();

    loadComplete = false;

    asyncBuffer.close();
    asyncBuffer.setData(QByteArray());
    asyncBuffer.open(QIODevice::ReadWrite);

    if (sequentialSourceDevice)
        sequentialSourceDevice->disconnect(q);
}

void QPdfDocumentPrivate::updateLastError()
{
    if (doc) {
        lastError = QPdfDocument::NoError;
        return;
    }

    QPdfMutexLocker lock;
    const unsigned long error = FPDF_GetLastError();
    lock.unlock();

    switch (error) {
    case FPDF_ERR_SUCCESS: lastError = QPdfDocument::NoError; break;
    case FPDF_ERR_UNKNOWN: lastError = QPdfDocument::UnknownError; break;
    case FPDF_ERR_FILE: lastError = QPdfDocument::FileNotFoundError; break;
    case FPDF_ERR_FORMAT: lastError = QPdfDocument::InvalidFileFormatError; break;
    case FPDF_ERR_PASSWORD: lastError = QPdfDocument::IncorrectPasswordError; break;
    case FPDF_ERR_SECURITY: lastError = QPdfDocument::UnsupportedSecuritySchemeError; break;
    default:
        Q_UNREACHABLE();
    }
}

void QPdfDocumentPrivate::load(QIODevice *newDevice, bool transferDeviceOwnership)
{
    if (transferDeviceOwnership)
        ownDevice.reset(newDevice);
    else
        ownDevice.reset();

    if (newDevice->isSequential()) {
        sequentialSourceDevice = newDevice;
        device = &asyncBuffer;
        QNetworkReply *reply = qobject_cast<QNetworkReply*>(sequentialSourceDevice);

        if (!reply) {
            setStatus(QPdfDocument::Error);
            qWarning() << "QPdfDocument: Loading from sequential devices only supported with QNetworkAccessManager.";
            return;
        }

        if (reply->isFinished() && reply->error() != QNetworkReply::NoError) {
            setStatus(QPdfDocument::Error);
            return;
        }

        QObject::connect(reply, &QNetworkReply::finished, q, [this, reply](){
            if (reply->error() != QNetworkReply::NoError || reply->bytesAvailable() == 0) {
                this->setStatus(QPdfDocument::Error);
            }
        });

        if (reply->header(QNetworkRequest::ContentLengthHeader).isValid())
            _q_tryLoadingWithSizeFromContentHeader();
        else
            QObject::connect(reply, SIGNAL(metaDataChanged()), q, SLOT(_q_tryLoadingWithSizeFromContentHeader()));
    } else {
        device = newDevice;
        initiateAsyncLoadWithTotalSizeKnown(device->size());
        checkComplete();
    }
}

void QPdfDocumentPrivate::_q_tryLoadingWithSizeFromContentHeader()
{
    if (avail)
        return;

    const QNetworkReply *networkReply = qobject_cast<QNetworkReply*>(sequentialSourceDevice);
    if (!networkReply) {
        setStatus(QPdfDocument::Error);
        return;
    }

    const QVariant contentLength = networkReply->header(QNetworkRequest::ContentLengthHeader);
    if (!contentLength.isValid()) {
        setStatus(QPdfDocument::Error);
        return;
    }

    QObject::connect(sequentialSourceDevice, SIGNAL(readyRead()), q, SLOT(_q_copyFromSequentialSourceDevice()));

    initiateAsyncLoadWithTotalSizeKnown(contentLength.toULongLong());

    if (sequentialSourceDevice->bytesAvailable())
        _q_copyFromSequentialSourceDevice();
}

void QPdfDocumentPrivate::initiateAsyncLoadWithTotalSizeKnown(quint64 totalSize)
{
    // FPDF_FILEACCESS setup
    m_FileLen = totalSize;

    const QPdfMutexLocker lock;

    avail = FPDFAvail_Create(this, this);
}

void QPdfDocumentPrivate::_q_copyFromSequentialSourceDevice()
{
    if (loadComplete)
        return;

    const QByteArray data = sequentialSourceDevice->read(sequentialSourceDevice->bytesAvailable());
    if (data.isEmpty())
        return;

    asyncBuffer.seek(asyncBuffer.size());
    asyncBuffer.write(data);

    checkComplete();
}

void QPdfDocumentPrivate::tryLoadDocument()
{
    QPdfMutexLocker lock;

    if (!FPDFAvail_IsDocAvail(avail, this))
        return;

    Q_ASSERT(!doc);

    doc = FPDFAvail_GetDocument(avail, password);
    lock.unlock();

    updateLastError();

    if (lastError == QPdfDocument::IncorrectPasswordError) {
        FPDF_CloseDocument(doc);
        doc = Q_NULLPTR;

        setStatus(QPdfDocument::Error);
        emit q->passwordRequired();
    }
}

void QPdfDocumentPrivate::checkComplete()
{
    if (!avail || loadComplete)
        return;

    if (!doc)
        tryLoadDocument();

    if (!doc)
        return;

    loadComplete = true;

    QPdfMutexLocker lock;

    for (int i = 0, count = FPDF_GetPageCount(doc); i < count; ++i) {
        int result = PDF_DATA_NOTAVAIL;
        while (result == PDF_DATA_NOTAVAIL) {
            result = FPDFAvail_IsPageAvail(avail, i, this);
        }

        if (result == PDF_DATA_ERROR)
            loadComplete = false;
    }

    lock.unlock();

    if (loadComplete)
        setStatus(QPdfDocument::Ready);
}

void QPdfDocumentPrivate::setStatus(QPdfDocument::Status documentStatus)
{
    if (status == documentStatus)
        return;

    status = documentStatus;
    emit q->statusChanged(status);
}

FPDF_BOOL QPdfDocumentPrivate::fpdf_IsDataAvail(_FX_FILEAVAIL *pThis, size_t offset, size_t size)
{
    QPdfDocumentPrivate *d = static_cast<QPdfDocumentPrivate*>(pThis);
    return offset + size <= static_cast<quint64>(d->device->size());
}

int QPdfDocumentPrivate::fpdf_GetBlock(void *param, unsigned long position, unsigned char *pBuf, unsigned long size)
{
    QPdfDocumentPrivate *d = static_cast<QPdfDocumentPrivate*>(reinterpret_cast<FPDF_FILEACCESS*>(param));
    d->device->seek(position);
    return qMax(qint64(0), d->device->read(reinterpret_cast<char *>(pBuf), size));

}

void QPdfDocumentPrivate::fpdf_AddSegment(_FX_DOWNLOADHINTS *pThis, size_t offset, size_t size)
{
    Q_UNUSED(pThis);
    Q_UNUSED(offset);
    Q_UNUSED(size);
}

QPdfDocument::QPdfDocument(QObject *parent)
    : QObject(parent)
    , d(new QPdfDocumentPrivate)
{
    d->q = this;
}

/*!
    Destroys the document.
*/
QPdfDocument::~QPdfDocument()
{
}

QPdfDocument::DocumentError QPdfDocument::load(const QString &fileName)
{
    close();

    d->setStatus(QPdfDocument::Loading);

    QScopedPointer<QFile> f(new QFile(fileName));
    if (!f->open(QIODevice::ReadOnly)) {
        d->lastError = FileNotFoundError;
        d->setStatus(QPdfDocument::Error);
    } else {
        d->load(f.take(), /*transfer ownership*/true);
    }
    return d->lastError;
}

/*!
    \enum QPdfDocument::Status

    This enum describes the current status of the document.

    \value Null The initial status after the document has been created or after it has been closed.
    \value Loading The status after load() has been called and before the document is fully loaded.
    \value Ready The status when the document is fully loaded and its data can be accessed.
    \value Unloading The status after close() has been called on an open document.
                     At this point the document is still valid and all its data can be accessed.
    \value Error The status after Loading, if loading has failed.

    \sa QPdfDocument::status()
*/

/*!
    Returns the current status of the document.
*/
QPdfDocument::Status QPdfDocument::status() const
{
    return d->status;
}

void QPdfDocument::load(QIODevice *device)
{
    close();

    d->setStatus(QPdfDocument::Loading);

    d->load(device, /*transfer ownership*/false);
}

void QPdfDocument::setPassword(const QString &password)
{
    const QByteArray newPassword = password.toUtf8();

    if (d->password == newPassword)
        return;

    d->password = newPassword;
    emit passwordChanged();
}

QString QPdfDocument::password() const
{
    return QString::fromUtf8(d->password);
}

/*!
    \enum QPdfDocument::MetaDataField

    This enum describes the available fields of meta data.

    \value Title The document's title as QString.
    \value Author The name of the person who created the document as QString.
    \value Subject The subject of the document as QString.
    \value Keywords Keywords associated with the document as QString.
    \value Creator If the document was converted to PDF from another format,
                   the name of the conforming product that created the original document
                   from which it was converted as QString.
    \value Producer If the document was converted to PDF from another format,
                    the name of the conforming product that converted it to PDF as QString.
    \value CreationDate The date and time the document was created as QDateTime.
    \value ModificationDate The date and time the document was most recently modified as QDateTime.

    \sa QPdfDocument::metaData()
*/

/*!
    Returns the meta data of the document for the given field.
*/
QVariant QPdfDocument::metaData(MetaDataField field) const
{
    if (!d->doc)
        return QString();

    QByteArray fieldName;
    switch (field) {
    case Title:
        fieldName = "Title";
        break;
    case Subject:
        fieldName = "Subject";
        break;
    case Author:
        fieldName = "Author";
        break;
    case Keywords:
        fieldName = "Keywords";
        break;
    case Producer:
        fieldName = "Producer";
        break;
    case Creator:
        fieldName = "Creator";
        break;
    case CreationDate:
        fieldName = "CreationDate";
        break;
    case ModificationDate:
        fieldName = "ModDate";
        break;
    }

    QPdfMutexLocker lock;
    const unsigned long len = FPDF_GetMetaText(d->doc, fieldName.constData(), Q_NULLPTR, 0);

    QVector<ushort> buf(len);
    FPDF_GetMetaText(d->doc, fieldName.constData(), buf.data(), buf.length());
    lock.unlock();

    QString text = QString::fromUtf16(buf.data());

    switch (field) {
    case Title: // fall through
    case Subject:
    case Author:
    case Keywords:
    case Producer:
    case Creator:
        return text;
    case CreationDate: // fall through
    case ModificationDate:
        // convert a "D:YYYYMMDDHHmmSSOHH'mm'" into "YYYY-MM-DDTHH:mm:ss+HH:mm"
        if (text.startsWith(QLatin1String("D:")))
            text = text.mid(2);
        text.insert(4, QLatin1Char('-'));
        text.insert(7, QLatin1Char('-'));
        text.insert(10, QLatin1Char('T'));
        text.insert(13, QLatin1Char(':'));
        text.insert(16, QLatin1Char(':'));
        text.replace(QLatin1Char('\''), QLatin1Char(':'));
        if (text.endsWith(QLatin1Char(':')))
            text.chop(1);

        return QDateTime::fromString(text, Qt::ISODate);
    }

    return QVariant();
}

QPdfDocument::DocumentError QPdfDocument::error() const
{
    return d->lastError;
}

/*!
    \fn void QPdfDocument::aboutToBeClosed()

    This signal is emitted whenever the document is closed.

    \sa close()
*/

/*!
  Closes the document.
*/
void QPdfDocument::close()
{
    if (!d->doc)
        return;

    d->setStatus(Unloading);

    d->clear();

    if (!d->password.isEmpty()) {
        d->password.clear();
        emit passwordChanged();
    }

    d->setStatus(Null);
}

int QPdfDocument::pageCount() const
{
    if (!d->doc)
        return 0;

    const QPdfMutexLocker lock;

    return FPDF_GetPageCount(d->doc);
}

QSizeF QPdfDocument::pageSize(int page) const
{
    QSizeF result;
    if (!d->doc)
        return result;

    const QPdfMutexLocker lock;

    FPDF_GetPageSizeByIndex(d->doc, page, &result.rwidth(), &result.rheight());
    return result;
}

QImage QPdfDocument::render(int page, const QSizeF &pageSize)
{
    if (!d->doc)
        return QImage();

    const QPdfMutexLocker lock;

    FPDF_PAGE pdfPage = FPDF_LoadPage(d->doc, page);
    if (!pdfPage)
        return QImage();

    QImage result(pageSize.toSize(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(result.width(), result.height(), FPDFBitmap_BGRA, result.bits(), result.bytesPerLine());

    FPDF_RenderPageBitmap(bitmap, pdfPage, 0, 0, result.width(), result.height(), 0, 0);

    FPDFBitmap_Destroy(bitmap);

    FPDF_ClosePage(pdfPage);

    return result;
}

QT_END_NAMESPACE

#include "moc_qpdfdocument.cpp"
