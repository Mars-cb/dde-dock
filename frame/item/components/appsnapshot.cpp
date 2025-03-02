// SPDX-FileCopyrightText: 2011 - 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "appsnapshot.h"
#include "previewcontainer.h"
#include "../widgets/tipswidget.h"
#include "utils.h"

#include <DStyle>

#include <X11/Xlib.h>
#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <sys/shm.h>

#include <QX11Info>
#include <QPainter>
#include <QVBoxLayout>
#include <QSizeF>
#include <QTimer>

struct SHMInfo {
    long shmid;
    long width;
    long height;
    long bytesPerLine;
    long format;

    struct Rect {
        long x;
        long y;
        long width;
        long height;
    } rect;
};

using namespace Dock;

AppSnapshot::AppSnapshot(const WId wid, QWidget *parent)
    : QWidget(parent)
    , m_wid(wid)
    , m_closeAble(false)
    , m_isWidowHidden(false)
    , m_title(new TipsWidget(this))
    , m_3DtitleBtn(nullptr)
    , m_waitLeaveTimer(new QTimer(this))
    , m_closeBtn2D(new DIconButton(this))
    , m_wmHelper(DWindowManagerHelper::instance())
    , m_dockDaemonInter(new DockDaemonInter("com.deepin.dde.daemon.Dock", "/com/deepin/dde/daemon/Dock", QDBusConnection::sessionBus(), this))
{
    m_closeBtn2D->setFixedSize(SNAP_CLOSE_BTN_WIDTH, SNAP_CLOSE_BTN_WIDTH);
    m_closeBtn2D->setIconSize(QSize(SNAP_CLOSE_BTN_WIDTH, SNAP_CLOSE_BTN_WIDTH));
    m_closeBtn2D->setObjectName("closebutton-2d");
    m_closeBtn2D->setIcon(QIcon(":/icons/resources/close_round_normal.svg"));
    m_closeBtn2D->setVisible(false);
    m_closeBtn2D->setFlat(true);
    m_closeBtn2D->installEventFilter(this);

    m_title->setObjectName("AppSnapshotTitle");

    QHBoxLayout *centralLayout = new QHBoxLayout;
    centralLayout->addWidget(m_title);
    centralLayout->setMargin(0);

    setLayout(centralLayout);
    setAcceptDrops(true);
    resize(SNAP_WIDTH / 2, SNAP_HEIGHT / 2);

    connect(m_closeBtn2D, &DIconButton::clicked, this, &AppSnapshot::closeWindow, Qt::QueuedConnection);
    connect(m_wmHelper, &DWindowManagerHelper::hasCompositeChanged, this, &AppSnapshot::compositeChanged, Qt::QueuedConnection);
    QTimer::singleShot(1, this, &AppSnapshot::compositeChanged);
}

void AppSnapshot::setWindowState()
{
    if (m_isWidowHidden) {
        m_dockDaemonInter->MinimizeWindow(m_wid);
    }
}

// 每次更新窗口信息时更新标题
void AppSnapshot::updateTitle()
{
    // 2D不显示
    if (!m_wmHelper->hasComposite())
        return;

    if (!m_3DtitleBtn) {
        m_3DtitleBtn = new DPushButton(this);
        m_3DtitleBtn->setAccessibleName("AppPreviewTitle");
        m_3DtitleBtn->setBackgroundRole(QPalette::Base);
        m_3DtitleBtn->setForegroundRole(QPalette::Text);
        m_3DtitleBtn->setFocusPolicy(Qt::NoFocus);
        m_3DtitleBtn->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_3DtitleBtn->setFixedHeight(36);
        m_3DtitleBtn->setVisible(false);
    }

    QFontMetrics fm(m_3DtitleBtn->font());
    int textWidth = fm.width(title()) + 10 + BTN_TITLE_MARGIN;
    int titleWidth = SNAP_WIDTH - (TITLE_MARGIN * 2  + BORDER_MARGIN);

    if (textWidth  < titleWidth) {
        m_3DtitleBtn->setFixedWidth(textWidth);
        m_3DtitleBtn->setText(title());
    } else {
        QString str = title();
        /*某些特殊字符只显示一半 如"Q"," W"，所以加一个空格保证字符显示完整,*/
        str.insert(0, " ");
        QString strTtile = m_3DtitleBtn->fontMetrics().elidedText(str, Qt::ElideRight, titleWidth - BTN_TITLE_MARGIN);
        m_3DtitleBtn->setText(strTtile);
        m_3DtitleBtn->setFixedWidth(titleWidth + BTN_TITLE_MARGIN);
    }

    // 移动到预览图中下
    m_3DtitleBtn->move(QPoint(SNAP_WIDTH / 2, SNAP_HEIGHT - m_3DtitleBtn->height() / 2 - TITLE_MARGIN) - m_3DtitleBtn->rect().center());
}

void AppSnapshot::setTitleVisible(bool bVisible)
{
    if (m_3DtitleBtn)
        m_3DtitleBtn->setVisible(bVisible && m_wmHelper->hasComposite());
}

void AppSnapshot::closeWindow() const
{
    if (Utils::IS_WAYLAND_DISPLAY) {
        m_dockDaemonInter->CloseWindow(static_cast<uint>(m_wid));
    } else {
        const auto display = QX11Info::display();
        if (!display) {
            qWarning() << "Error: get display failed!";
            return;
        }

        XEvent e;

        memset(&e, 0, sizeof(e));
        e.xclient.type = ClientMessage;
        e.xclient.window = m_wid;
        e.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", true);
        e.xclient.format = 32;
        e.xclient.data.l[0] = XInternAtom(display, "WM_DELETE_WINDOW", false);
        e.xclient.data.l[1] = CurrentTime;

        Q_EMIT requestCloseAppSnapshot();

        XSendEvent(display, m_wid, false, NoEventMask, &e);
        XFlush(display);
    }
}

void AppSnapshot::compositeChanged() const
{
    const bool composite = m_wmHelper->hasComposite();

    m_title->setVisible(!composite);

    QTimer::singleShot(1, this, &AppSnapshot::fetchSnapshot);
}

void AppSnapshot::setWindowInfo(const WindowInfo &info)
{
    m_windowInfo = info;
    QFontMetrics fm(m_title->font());
    QString strTtile = m_title->fontMetrics().elidedText(m_windowInfo.title, Qt::ElideRight, SNAP_WIDTH - SNAP_CLOSE_BTN_WIDTH - SNAP_CLOSE_BTN_MARGIN);
    m_title->setText(strTtile);
    updateTitle();

    // 只有在X11下，才能通过XGetWindowProperty获取窗口属性
    if (qEnvironmentVariable("XDG_SESSION_TYPE").contains("x11")) {
        getWindowState();
    }
}

void AppSnapshot::dragEnterEvent(QDragEnterEvent *e)
{
    QWidget::dragEnterEvent(e);

    if (m_wmHelper->hasComposite())
        emit entered(m_wid);
}

void AppSnapshot::fetchSnapshot()
{
    if (!m_wmHelper->hasComposite())
        return;

    QImage qimage;
    SHMInfo *info = nullptr;
    uchar *image_data = nullptr;
    XImage *ximage = nullptr;

    do {
        // 优先使用窗管进行窗口截图
        if (isKWinAvailable()) {
            QDBusInterface interface(QStringLiteral("org.kde.KWin"), QStringLiteral("/Screenshot"), QStringLiteral("org.kde.kwin.Screenshot"));
            qDebug() << "windowsID:"<< m_wid;

            QList<QVariant> args;
            args << QVariant::fromValue(m_wid);
            args << QVariant::fromValue(quint32(SNAP_WIDTH));
            args << QVariant::fromValue(quint32(SNAP_HEIGHT));

            QDBusReply<QString> reply = interface.callWithArgumentList(QDBus::Block,QStringLiteral("screenshotForWindowExtend"), args);
            if(reply.isValid()){
                const QString tmpFile = reply.value();
                if (QFile::exists(tmpFile)) {
                    m_snapshot.load(tmpFile);
                    m_snapshotSrcRect = m_snapshot.rect();
                    qDebug() << "reply: " << tmpFile;
                    QFile::remove(tmpFile);
                    break;
                }  else {
                    qDebug() << "get current workspace bckground error, file does not exist : " << tmpFile;
                }
            } else {
                qDebug() << "get current workspace bckground error: "<< reply.error().message();
            }
        }

        // get window image from shm(only for deepin app)
        info = getImageDSHM();
        if (info) {
            qDebug() << "get Image from dxcbplugin SHM...";
            image_data = (uchar *)shmat(info->shmid, 0, 0);
            if ((qint64)image_data != -1) {
                m_snapshot = QImage(image_data, info->width, info->height, info->bytesPerLine, (QImage::Format)info->format);
                m_snapshotSrcRect = QRect(info->rect.x, info->rect.y, info->rect.width, info->rect.height);
                break;
            }
            qDebug() << "invalid pointer of shm!";
            image_data = nullptr;
        }

        if (!Utils::IS_WAYLAND_DISPLAY && (!image_data || qimage.isNull())) {
            // get window image from XGetImage(a little slow)
            qDebug() << "get Image from dxcbplugin SHM failed!";
            qDebug() << "get Image from Xlib...";
            // guoyao note：这里会造成内存泄漏，而且是通过demo在X环境经过验证，改用xcb库同样会有内存泄漏，这里暂时未找到解决方案，所以优先使用kwin提供的接口
            ximage = getImageXlib();
            if (!ximage) {
                qDebug() << "get Image from Xlib failed! giving up...";
                emit requestCheckWindow();
                return;
            }
            qimage = QImage((const uchar *)(ximage->data), ximage->width, ximage->height, ximage->bytes_per_line, QImage::Format_RGB32);

            if (!qimage.isNull()) {
                m_snapshot = qimage;
                // remove shadow frame
                m_snapshotSrcRect = rectRemovedShadow(qimage, nullptr);
            } else {
                qDebug() << "can not get QImage! giving up...";
            }
        }
    } while(false);

    // 如果m_snapshot或m_snapshotSrcRect为空，说明三种方式均失败，返回不做处理
    if (m_snapshot.isNull() || m_snapshotSrcRect.isNull()) {
        qWarning() << "can not get QImage or QRectF! giving up...";
        return;
    }

    QSizeF size(rect().marginsRemoved(QMargins(8, 8, 8, 8)).size());
    const auto ratio = devicePixelRatioF();
    size = m_snapshotSrcRect.size().scaled(size * ratio, Qt::KeepAspectRatio);
    qreal scale = qreal(size.width()) / m_snapshotSrcRect.width();
    m_snapshot = m_snapshot.scaled(qRound(m_snapshot.width() * scale), qRound(m_snapshot.height() * scale),
                                   Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_snapshotSrcRect.moveTop(m_snapshotSrcRect.top() * scale + 0.5);
    m_snapshotSrcRect.moveLeft(m_snapshotSrcRect.left() * scale + 0.5);
    m_snapshotSrcRect.setWidth(size.width() - 0.5);
    m_snapshotSrcRect.setHeight(size.height() - 0.5);

    if (image_data) shmdt(image_data);
    if (ximage) XDestroyImage(ximage);
    if (info) XFree(info);

    update();
}

void AppSnapshot::enterEvent(QEvent *e)
{
    QWidget::enterEvent(e);

    if (!m_wmHelper->hasComposite()) {
        m_closeBtn2D->move(width() - m_closeBtn2D->width() - SNAP_CLOSE_BTN_MARGIN, (height() - m_closeBtn2D->height()) / 2);
        m_closeBtn2D->setVisible(true);
    } else {
        emit entered(wid());
    }

    update();
}

void AppSnapshot::leaveEvent(QEvent *e)
{
    QWidget::leaveEvent(e);

    m_closeBtn2D->setVisible(false);

    update();
}

void AppSnapshot::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);

    QPainter painter(this);

    if (!m_wmHelper->hasComposite()) {
        if (underMouse())
            painter.fillRect(rect(), QColor(255, 255, 255, 255 * .2));
        return;
    }

    if (m_snapshot.isNull())
        return;

    const auto ratio = devicePixelRatioF();

    // draw attention background
    if (m_windowInfo.attention) {
        painter.setBrush(QColor(241, 138, 46, 255 * .8));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(rect(), 5, 5);
    }

    // draw image
    const QImage &im = m_snapshot;

    const qreal offset_x = width() / 2.0 - m_snapshotSrcRect.width() / ratio / 2 - m_snapshotSrcRect.left() / ratio;
    const qreal offset_y = height() / 2.0 - m_snapshotSrcRect.height() / ratio / 2 - m_snapshotSrcRect.top() / ratio;

    DStyleHelper dstyle(style());
    const int radius = dstyle.pixelMetric(DStyle::PM_FrameRadius);

    QBrush brush;
    brush.setTextureImage(im);
    painter.setBrush(brush);
    painter.setPen(Qt::NoPen);
    painter.scale(1 / ratio, 1 / ratio);
    painter.translate(QPoint(offset_x * ratio, offset_y * ratio));
    painter.drawRoundedRect(m_snapshotSrcRect, radius * ratio, radius * ratio);
}

void AppSnapshot::mousePressEvent(QMouseEvent *e)
{
    QWidget::mousePressEvent(e);

    emit clicked(m_wid);
}

bool AppSnapshot::eventFilter(QObject *watched, QEvent *e)
{
    if (watched == m_closeBtn2D) {
        // TODO 判断条件重复
        if (watched == m_closeBtn2D && (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverMove)) {
            m_closeBtn2D->setIcon(QIcon(":/icons/resources/close_round_hover.svg"));
        } else if (watched == m_closeBtn2D && e->type() == QEvent::HoverLeave) {
            m_closeBtn2D->setIcon(QIcon(":/icons/resources/close_round_normal.svg"));
        } else if (watched == m_closeBtn2D && e->type() == QEvent::MouseButtonPress) {
            m_closeBtn2D->setIcon(QIcon(":/icons/resources/close_round_press.svg"));
        }
    }

    return false;
}

void AppSnapshot::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    fetchSnapshot();
}

SHMInfo *AppSnapshot::getImageDSHM()
{
    const auto display = Utils::IS_WAYLAND_DISPLAY ? XOpenDisplay(nullptr) : QX11Info::display();
    if (!display) {
        qWarning() << "Error: get display failed!";
        return nullptr;
    }

    Atom atom_prop = XInternAtom(display, "_DEEPIN_DXCB_SHM_INFO", true);
    if (!atom_prop) {
        return nullptr;
    }

    Atom actual_type_return_deepin_shm;
    int actual_format_return_deepin_shm;
    unsigned long nitems_return_deepin_shm;
    unsigned long bytes_after_return_deepin_shm;
    unsigned char *prop_return_deepin_shm;

    XGetWindowProperty(display, m_wid, atom_prop, 0, 32 * 9, false, AnyPropertyType,
                       &actual_type_return_deepin_shm, &actual_format_return_deepin_shm, &nitems_return_deepin_shm,
                       &bytes_after_return_deepin_shm, &prop_return_deepin_shm);

    //qDebug() << actual_type_return_deepin_shm << actual_format_return_deepin_shm << nitems_return_deepin_shm << bytes_after_return_deepin_shm << prop_return_deepin_shm;

    return reinterpret_cast<SHMInfo *>(prop_return_deepin_shm);
}

XImage *AppSnapshot::getImageXlib()
{
    const auto display = Utils::IS_WAYLAND_DISPLAY ? XOpenDisplay(nullptr) : QX11Info::display();
    if (!display) {
        qWarning() << "Error: get display failed!";
        return nullptr;
    }

    Window unused_window;
    int unused_int;
    unsigned unused_uint, w, h;
    XGetGeometry(display, m_wid, &unused_window, &unused_int, &unused_int, &w, &h, &unused_uint, &unused_uint);
    return XGetImage(display, m_wid, 0, 0, w, h, AllPlanes, ZPixmap);
}

QRect AppSnapshot::rectRemovedShadow(const QImage &qimage, unsigned char *prop_to_return_gtk)
{
    const auto display = Utils::IS_WAYLAND_DISPLAY ? XOpenDisplay(nullptr) : QX11Info::display();
    if (!display) {
        qWarning() << "Error: get display failed!";
        return QRect();
    }

    const Atom gtk_frame_extents = XInternAtom(display, "_GTK_FRAME_EXTENTS", true);
    Atom actual_type_return_gtk;
    int actual_format_return_gtk;
    unsigned long n_items_return_gtk;
    unsigned long bytes_after_return_gtk;

    const auto r = XGetWindowProperty(display, m_wid, gtk_frame_extents, 0, 4, false, XA_CARDINAL,
                                      &actual_type_return_gtk, &actual_format_return_gtk, &n_items_return_gtk, &bytes_after_return_gtk, &prop_to_return_gtk);
    if (!r && prop_to_return_gtk && n_items_return_gtk == 4 && actual_format_return_gtk == 32) {
        qDebug() << "remove shadow frame...";
        const unsigned long *extents = reinterpret_cast<const unsigned long *>(prop_to_return_gtk);
        const int left = extents[0];
        const int right = extents[1];
        const int top = extents[2];
        const int bottom = extents[3];
        const int width = qimage.width();
        const int height = qimage.height();

        return QRect(left, top, width - left - right, height - top - bottom);
    } else {
        return QRect(0, 0, qimage.width(), qimage.height());
    }
}

void AppSnapshot::getWindowState()
{
    Atom actual_type;
    int actual_format;
    unsigned long i, num_items, bytes_after;
    unsigned char *properties = nullptr;

    m_isWidowHidden = false;

    const auto display = Utils::IS_WAYLAND_DISPLAY ? XOpenDisplay(nullptr) : QX11Info::display();
    if (!display) {
        qWarning() << "Error: get display failed!";
        return;
    }
    Atom atom_prop = XInternAtom(display, "_NET_WM_STATE", true);
    if (!atom_prop) {
        return;
    }

    Status status = XGetWindowProperty(display, m_wid, atom_prop, 0, LONG_MAX, False, AnyPropertyType, &actual_type, &actual_format, &num_items, &bytes_after, &properties);
    if (status != Success) {
        qDebug() << "Fail to get window state";
        return;
    }

    Atom *atoms = reinterpret_cast<Atom *>(properties);
    for (i = 0; i < num_items; ++i) {
        const char *atomName = XGetAtomName(display, atoms[i]);

        if (strcmp(atomName, "_NET_WM_STATE_HIDDEN") == 0) {
            m_isWidowHidden = true;
            break;
        }
    }

    if (properties) {
        XFree(properties);
    }
}

bool AppSnapshot::isKWinAvailable()
{
    if (QDBusConnection::sessionBus().interface()->isServiceRegistered(QStringLiteral("org.kde.KWin"))) {
        QDBusInterface interface(QStringLiteral("org.kde.KWin"), QStringLiteral("/Effects"), QStringLiteral("org.kde.kwin.Effects"));
        QDBusReply<bool> reply = interface.call(QStringLiteral("isEffectLoaded"), "screenshot");

        return reply.value();
    }
    return false;
}
