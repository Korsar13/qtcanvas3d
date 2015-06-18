/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtCanvas3D module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL3$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPLv3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or later as published by the Free
** Software Foundation and appearing in the file LICENSE.GPL included in
** the packaging of this file. Please review the following information to
** ensure the GNU General Public License version 2.0 requirements will be
** met: http://www.gnu.org/licenses/gpl-2.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "canvas3d_p.h"
#include "context3d_p.h"
#include "canvas3dcommon_p.h"
#include "canvasrendernode_p.h"
#include "teximage3d_p.h"
#include "glcommandqueue_p.h"
#include "canvasglstatedump_p.h"
#include "renderjob_p.h"
#include "canvasrenderer_p.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlContext>
#include <QtCore/QElapsedTimer>

QT_BEGIN_NAMESPACE
QT_CANVAS3D_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(canvas3dinfo, "qt.canvas3d.info")
Q_LOGGING_CATEGORY(canvas3drendering, "qt.canvas3d.rendering")
Q_LOGGING_CATEGORY(canvas3dglerrors, "qt.canvas3d.glerrors")

/*!
 * \qmltype Canvas3D
 * \since QtCanvas3D 1.0
 * \inqmlmodule QtCanvas3D
 * \brief Canvas that provides a 3D rendering context.
 *
 * The Canvas3D is a QML element that, when placed in your Qt Quick 2 scene, allows you to
 * get a 3D rendering context and call 3D rendering API calls through that context object.
 * Use of the rendering API requires knowledge of OpenGL like rendering APIs.
 *
 * There are two functions that are called by the Canvas3D implementation:
 * \list
 * \li initializeGL is emitted before the first frame is rendered, and usually during that you get
 * the 3D context and initialize resources to be used later on during the rendering cycle.
 * \li paintGL is emitted for each frame to be rendered, and usually during that you
 * submit 3D rendering calls to draw whatever 3D content you want to be displayed.
 * \endlist
 *
 * \sa Context3D
 */

/*!
 * \internal
 */
Canvas::Canvas(QQuickItem *parent):
    QQuickItem(parent),
    m_isNeedRenderQueued(false),
    m_rendererReady(false),
    m_context3D(0),
    m_fboSize(0, 0),
    m_maxSize(0, 0),
    m_maxSamples(0),
    m_devicePixelRatio(1.0f),
    m_isOpenGLES2(false),
    m_isSoftwareRendered(false),
    m_isContextAttribsSet(false),
    m_resizeGLQueued(false),
    m_firstSync(true),
    m_renderMode(RenderModeOffscreenBuffer),
    m_renderer(0),
    m_maxVertexAttribs(0),
    m_contextVersion(0)
{
    connect(this, &QQuickItem::windowChanged, this, &Canvas::handleWindowChanged);
    connect(this, &Canvas::needRender, this, &Canvas::queueNextRender, Qt::QueuedConnection);
    connect(this, &QQuickItem::widthChanged, this, &Canvas::queueResizeGL, Qt::DirectConnection);
    connect(this, &QQuickItem::heightChanged, this, &Canvas::queueResizeGL, Qt::DirectConnection);
    setAntialiasing(false);

    // Set contents to false in case we are in qml designer to make component look nice
    m_runningInDesigner = QGuiApplication::applicationDisplayName() == "Qml2Puppet";
    setFlag(ItemHasContents, !(m_runningInDesigner || m_renderMode != RenderModeOffscreenBuffer));

#if (QT_VERSION >= QT_VERSION_CHECK(5, 4, 0))
    if (QCoreApplication::testAttribute(Qt::AA_UseSoftwareOpenGL))
        m_isSoftwareRendered = true;
#endif
}

/*!
 * \qmlsignal void Canvas3D::initializeGL()
 * Emitted once when Canvas3D is ready and OpenGL state initialization can be done by the client.
 */

/*!
 * \qmlsignal void Canvas3D::paintGL()
 * Emitted each time a new frame should be drawn to Canvas3D.
 * Driven by the Qt Quick scenegraph loop.
 */

/*!
 * \internal
 */
Canvas::~Canvas()
{
    // Ensure that all JS objects have been destroyed before we destroy the command queue.
    delete m_context3D;

    if (m_renderer) {
        if (m_renderer->thread() == QThread::currentThread())
            delete m_renderer;
        else
            m_renderer->deleteLater();
    }
}

/*!
 * \internal
 *
 * Override QQuickItem's setWidth to be able to limit the maximum canvas size to maximum viewport
 * dimensions.
 */
void Canvas::setWidth(int width)
{
    int newWidth = width;
    int maxWidth = m_maxSize.width();
    if (maxWidth && width > maxWidth) {
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << "():"
                                             << "Maximum width exceeded. Limiting to "
                                             << maxWidth;
        newWidth = maxWidth;
    }

    QQuickItem::setWidth(newWidth);
}

/*!
 * \internal
 */
int Canvas::width()
{
    return QQuickItem::width();
}

/*!
 * \internal
 *
 * Override QQuickItem's setHeight to be able to limit the maximum canvas size to maximum viewport
 * dimensions.
 */
void Canvas::setHeight(int height)
{
    int newHeight = height;
    int maxHeight = m_maxSize.height();
    if (maxHeight && height > maxHeight) {
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << "():"
                                             << "Maximum height exceeded. Limiting to "
                                             << maxHeight;
        newHeight = maxHeight;
    }

    QQuickItem::setHeight(newHeight);
}

/*!
 * \internal
 */
int Canvas::height()
{
    return QQuickItem::height();
}

/*!
 * \qmlproperty RenderMode Canvas3D::renderMode
 * Specifies how the rendering should be done.
 * \list
 * \li \c Canvas3D.RenderModeOffscreenBuffer indicates rendering is done into an offscreen
 * buffer and the finished texture is used for the Canvas3D item. This is the default mode.
 * \li \c Canvas3D.RenderModeBackground indicates the rendering is done to the background of the
 * Qt Quick scene, in response to QQuickWindow::beforeRendering() signal.
 * \li \c Canvas3D.RenderModeForeground indicates the rendering is done to the foreground of the
 * Qt Quick scene, in response to QQuickWindow::afterRendering() signal.
 * \endlist
 *
 * \c Canvas3D.RenderModeBackground and \c Canvas3D.RenderModeForeground modes render directly to
 * the same framebuffer the rest of the Qt Quick scene uses. This will improve performance
 * on platforms that are fill-rate limited, but using these modes imposes several limitations
 * on the usage of Canvas3D:
 *
 * \list
 * \li Only Canvas3D items that fill the entire window are supported. Note that you can still
 * control the actual rendering area by using an appropriate viewport.
 * \li Antialiasing is only supported if the surface format of the window supports multisampling.
 * You may need to specify the surface format of the window explicitly in your \c{main.cpp}.
 * \li The default framebuffer needs to be cleared every time before the Qt Quick scene renders a
 * frame. This means that you cannot use synchronous Context3D commands after any draw command
 * targeting the default framebuffer in your \l{Canvas3D::paintGL}{Canvas3D.paintGL()} signal
 * handler, as such commands cause drawing to happen before the default framebuffer is cleared
 * for that frame.
 * A synchronous command is any Context3D command that requires waiting for GL command queue
 * to finish executing before it returns, such as \l{Context3D::getError}{Context3D.getError()}
 * or \l{Context3D::readPixels}{Context3D.readPixels()}. When in doubt, see the individual command
 * documentation to see if that command is considered synchronous.
 * \li When drawing to the foreground, you shouldn't issue a
 * \l{Context3D::clear}{Context3D.clear(Context3D.GL_COLOR_BUFFER_BIT)} command targeting the
 * default framebuffer in your \l{Canvas3D::paintGL}{Canvas3D.paintGL()} signal handler,
 * as that will clear all other Qt Quick items from the scene. Clearing depth and stencil buffers
 * is allowed.
 * \li You lose the ability to control the z-order of the Canvas3D item itself, as it is always
 * drawn either behind or in front of all other Qt Quick items.
 * \li The context attributes given as Canvas3D.getContext() parameters are ignored and the
 * corresponding values of the Qt Quick context are used.
 * \li Drawing to the background or the foreground doesn't work when Qt Quick is using OpenGL
 * core profile, as Canvas3D requires either OpenGL 2.x compatibility or OpenGL ES2.
 * \endlist
 *
 * This property can only be modified before the Canvas3D item has been rendered for the first time.
 */
void Canvas::setRenderMode(RenderMode mode)
{
    if (m_firstSync) {
        RenderMode oldMode = m_renderMode;
        m_renderMode = mode;
        if (m_renderMode == RenderModeOffscreenBuffer)
            setFlag(ItemHasContents, true);
        else
            setFlag(ItemHasContents, false);
        if (oldMode != m_renderMode)
            emit renderModeChanged();
    } else {
        qCWarning(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                               << ": renderMode property can only be "
                                               << "modified before Canvas3D item is rendered the "
                                               << "first time";
    }
}

Canvas::RenderMode Canvas::renderMode() const
{
    return m_renderMode;
}

/*!
 * \qmlproperty float Canvas3D::devicePixelRatio
 * Specifies the ratio between logical pixels (used by the Qt Quick) and actual physical
 * on-screen pixels (used by the 3D rendering).
 */
float Canvas::devicePixelRatio()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";
    QQuickWindow *win = window();
    if (win)
        return win->devicePixelRatio();
    else
        return 1.0f;
}


/*!
 * \qmlmethod Context3D Canvas3D::getContext(string type)
 * Returns the 3D rendering context that allows 3D rendering calls to be made.
 * The \a type parameter is ignored for now, but a string is expected to be given.
 */
/*!
 * \internal
 */
QJSValue Canvas::getContext(const QString &type)
{
    QVariantMap map;
    return getContext(type, map);
}

/*!
 * \qmlmethod Context3D Canvas3D::getContext(string type, Canvas3DContextAttributes options)
 * Returns the 3D rendering context that allows 3D rendering calls to be made.
 * The \a type parameter is ignored for now, but a string is expected to be given.
 * If Canvas3D.renderMode property value is either \c Canvas3D.RenderModeBackground or
 * \c Canvas3D.RenderModeForeground, the \a options parameter is also ignored,
 * the context attributes of the Qt Quick context are used, and the
 * \l{Canvas3DContextAttributes::preserveDrawingBuffer}{Canvas3DContextAttributes.preserveDrawingBuffer}
 * property is forced to \c{false}.
 * The \a options parameter is only parsed when the first call to getContext() is
 * made and is ignored in subsequent calls if given. If the first call is made without
 * giving the \a options parameter, then the context and render target is initialized with
 * default configuration.
 *
 * \sa Canvas3DContextAttributes, Context3D, renderMode
 */
/*!
 * \internal
 */
QJSValue Canvas::getContext(const QString &type, const QVariantMap &options)
{
    Q_UNUSED(type);

    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                         << "(type:" << type
                                         << ", options:" << options
                                         << ")";

    if (!m_isContextAttribsSet) {
        // Accept passed attributes only from first call and ignore for subsequent calls
        m_isContextAttribsSet = true;
        m_contextAttribs.setFrom(options);

        qCDebug(canvas3drendering).nospace()  << "Canvas3D::" << __FUNCTION__
                                              << " Context attribs:" << m_contextAttribs;

        // If we can't do antialiasing, ensure we don't even try to enable it
        if (m_maxSamples == 0 || m_isSoftwareRendered)
            m_contextAttribs.setAntialias(false);

        // Reflect the fact that creation of stencil attachment
        // causes the creation of depth attachment as well
        if (m_contextAttribs.stencil())
            m_contextAttribs.setDepth(true);

        // Ensure ignored attributes are left to their default state
        m_contextAttribs.setPremultipliedAlpha(false);
        m_contextAttribs.setPreferLowPowerToHighPerformance(false);
        m_contextAttribs.setFailIfMajorPerformanceCaveat(false);
    }

    if (!m_renderer->contextCreated()) {
        updateWindowParameters();

        if (!m_renderer->createContext(window(), m_contextAttribs, m_maxVertexAttribs, m_maxSize,
                                       m_contextVersion, m_extensions)) {
            return QJSValue(QJSValue::NullValue);
        }

        setPixelSize(m_renderer->fboSize());
    }

    if (!m_context3D) {
        m_context3D = new CanvasContext(QQmlEngine::contextForObject(this)->engine(),
                                        m_isOpenGLES2, m_maxVertexAttribs,
                                        m_contextVersion, m_extensions,
                                        m_renderer->commandQueue());

        connect(m_renderer, &CanvasRenderer::textureIdResolved,
                m_context3D, &CanvasContext::handleTextureIdResolved,
                Qt::QueuedConnection);

        // Verify that width and height are not initially too large, in case width and height
        // were set before getting GL_MAX_VIEWPORT_DIMS
        if (width() > m_maxSize.width()) {
            qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                                 << "():"
                                                 << "Maximum width exceeded. Limiting to "
                                                 << m_maxSize.width();
            QQuickItem::setWidth(m_maxSize.width());
        }
        if (height() > m_maxSize.height()) {
            qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                                 << "():"
                                                 << "Maximum height exceeded. Limiting to "
                                                 << m_maxSize.height();
            QQuickItem::setHeight(m_maxSize.height());
        }

        m_context3D->setCanvas(this);
        m_context3D->setDevicePixelRatio(m_devicePixelRatio);
        m_context3D->setContextAttributes(m_contextAttribs);

        emit contextChanged(m_context3D);
    }

    return QQmlEngine::contextForObject(this)->engine()->newQObject(m_context3D);
}

/*!
 * \qmlproperty size Canvas3D::pixelSize
 * Specifies the size of the render target surface in physical on-screen pixels used by
 * the 3D rendering.
 */
/*!
 * \internal
 */
QSize Canvas::pixelSize()
{
    return m_fboSize;
}

/*!
 * \internal
 */
void Canvas::setPixelSize(QSize pixelSize)
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                         << "(pixelSize:" << pixelSize
                                         << ")";

    if (pixelSize.width() > m_maxSize.width()) {
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << "():"
                                             << "Maximum pixel width exceeded limiting to "
                                             << m_maxSize.width();
        pixelSize.setWidth(m_maxSize.width());
    }

    if (pixelSize.height() > m_maxSize.height()) {
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << "():"
                                             << "Maximum pixel height exceeded limiting to "
                                             << m_maxSize.height();
        pixelSize.setHeight(m_maxSize.height());
    }

    if (m_renderer)
        m_renderer->setFboSize(pixelSize);

    if (m_fboSize == pixelSize)
        return;

    m_fboSize = pixelSize;

    // Queue the pixel size signal to next repaint cycle and queue repaint
    queueResizeGL();
    emitNeedRender();
    emit pixelSizeChanged(pixelSize);
}

/*!
 * \internal
 */
void Canvas::handleWindowChanged(QQuickWindow *window)
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "(" << window << ")";
    if (!window)
        return;

    if (m_renderMode != RenderModeOffscreenBuffer) {
        connect(window, &QQuickWindow::beforeSynchronizing,
                this, &Canvas::handleBeforeSynchronizing, Qt::DirectConnection);
        window->setClearBeforeRendering(false);
    }

    emitNeedRender();
}

/*!
 * \internal
 */
void Canvas::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                         << "(newGeometry:" << newGeometry
                                         << ", oldGeometry" << oldGeometry
                                         << ")";
    QQuickItem::geometryChanged(newGeometry, oldGeometry);

    emitNeedRender();
}

/*!
 * \internal
 */
void Canvas::itemChange(ItemChange change, const ItemChangeData &value)
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                         << "(change:" << change
                                         << ")";
    QQuickItem::itemChange(change, value);

    emitNeedRender();
}

/*!
 * \internal
 */
CanvasContext *Canvas::context()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";
    return m_context3D;
}

/*!
 * \internal
 */
void Canvas::updateWindowParameters()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";

    // Update the device pixel ratio
    QQuickWindow *win = window();

    if (win) {
        qreal pixelRatio = win->devicePixelRatio();
        if (pixelRatio != m_devicePixelRatio) {
            m_devicePixelRatio = pixelRatio;
            emit devicePixelRatioChanged(pixelRatio);
            queueResizeGL();
            win->update();
        }
    }

    if (m_context3D) {
        if (m_context3D->devicePixelRatio() != m_devicePixelRatio)
            m_context3D->setDevicePixelRatio(m_devicePixelRatio);
    }
}

void Canvas::sync()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";

    // Update execution queue (GUI thread is locked here)
    m_renderer->transferCommands();

    // Start queuing up another frame
    emitNeedRender();
}

bool Canvas::firstSync()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";

    if (!m_renderer) {
        m_renderer = new CanvasRenderer();

        connect(m_renderer, &CanvasRenderer::fpsChanged,
                this, &Canvas::fpsChanged);
    }

    if (!m_renderer->qtContextResolved()) {
        m_firstSync = false;
        QSize initializedSize = boundingRect().size().toSize();
        m_renderer->resolveQtContext(window(), initializedSize, m_renderMode);
        m_isOpenGLES2 = m_renderer->isOpenGLES2();

        if (m_renderMode != RenderModeOffscreenBuffer) {
            m_renderer->getQtContextAttributes(m_contextAttribs);
            m_isContextAttribsSet = true;
            m_renderer->init(window(), m_contextAttribs, m_maxVertexAttribs, m_maxSize,
                             m_contextVersion, m_extensions);
            setPixelSize(m_renderer->fboSize());
        } else {
            m_renderer->createContextShare();
            m_maxSamples = m_renderer->maxSamples();
        }

        connect(window(), &QQuickWindow::sceneGraphInvalidated,
                m_renderer, &CanvasRenderer::shutDown, Qt::DirectConnection);

        if (m_renderMode == RenderModeForeground) {
            connect(window(), &QQuickWindow::beforeRendering,
                    m_renderer, &CanvasRenderer::clearBackground, Qt::DirectConnection);
            connect(window(), &QQuickWindow::afterRendering,
                    m_renderer, &CanvasRenderer::render, Qt::DirectConnection);
        } else {
            connect(window(), &QQuickWindow::beforeRendering,
                    m_renderer, &CanvasRenderer::render, Qt::DirectConnection);
        }

        return true;
    }
    return false;
}

/*!
 * \internal
 */
CanvasRenderer *Canvas::renderer()
{
    return m_renderer;
}

/*!
 * \internal
 */
QSGNode *Canvas::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data)
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                         << "("
                                         << oldNode <<", " << data
                                         << ")";
    updateWindowParameters();
    QSize initializedSize = boundingRect().size().toSize();
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                         << " size:" << initializedSize
                                         << " devicePixelRatio:" << m_devicePixelRatio;
    if (m_runningInDesigner
            || initializedSize.width() < 0
            || initializedSize.height() < 0
            || !window()) {
        delete oldNode;
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << " Returns null";

        m_rendererReady = false;
        return 0;
    }

    CanvasRenderNode *node = static_cast<CanvasRenderNode *>(oldNode);

    if (firstSync()) {
        update();
        return 0;
    }

    if (!node) {
        node = new CanvasRenderNode(window());

        /* Set up connections to get the production of FBO textures in sync with vsync on the
         * main thread.
         *
         * When the OpenGL commands for rendering the new texture are queued in queueNextRender(),
         * QQuickItem::update() is called to trigger updatePaintNode() call (this function).
         * QQuickWindow::update() is also queued to actually cause the redraw to happen.
         *
         * The queued OpenGL commands are transferred into the execution queue below. This is safe
         * because GUI thread is blocked at this point. After we have transferred the commands
         * for execution, we emit needRender() signal to trigger queueing of the commands for
         * the next frame.
         *
         * The queued commands are actually executed in the render thread in response to
         * QQuickWindow::beforeRendering() signal, which is connected to
         * CanvasRenderer::render() slot.
         *
         * When executing commands, an internalTextureComplete command indicates a complete frame.
         * The render buffers are swapped at that point and the node texture is updated via direct
         * connected CanvasRenderer::textureReady() signal.
         *
         * This rendering pipeline is throttled by vsync on the scene graph rendering thread.
         */
        connect(m_renderer, &CanvasRenderer::textureReady,
                node, &CanvasRenderNode::newTexture,
                Qt::DirectConnection);

        m_rendererReady = true;
    }

    sync();

    node->setRect(boundingRect());

    return node;
}

/*!
 * \qmlproperty uint Canvas3D::fps
 * This property specifies the current frames per seconds, the value is calculated every
 * 500 ms.
 */
uint Canvas::fps()
{
    return m_renderer ? m_renderer->fps() : 0;
}

/*!
 * \internal
 */
void Canvas::queueNextRender()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";

    m_isNeedRenderQueued = false;

    updateWindowParameters();

    // Don't try to do anything before the renderer/node are ready
    if (!m_rendererReady) {
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << " Renderer not ready, returning";
        return;
    }

    // Check that we're complete component before drawing
    if (!isComponentComplete()) {
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << " Component is not complete, skipping drawing";
        return;
    }

    if (!m_context3D) {
        // Call the initialize function from QML/JavaScript. It'll call the getContext()
        // that in turn creates the renderer context.
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << " Emit initializeGL() signal";

        // Call init on JavaScript side to queue the user's GL initialization commands.
        // The initial context creation get will also initialize the context command queue.
        emit initializeGL();

        if (!m_isContextAttribsSet) {
            qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                                 << " Context attributes not set, returning";
            return;
        }

        if (!m_renderer->contextCreated()) {
            qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                                 << " QOpenGLContext not created, returning";
            return;
        }
    }

    // Signal changes in pixel size
    if (m_resizeGLQueued) {
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << " Emit resizeGL() signal";
        emit resizeGL(int(width()), int(height()), m_devicePixelRatio);
        m_resizeGLQueued = false;
    }

    // Check if any images are loaded and need to be notified
    QQmlEngine *engine = QQmlEngine::contextForObject(this)->engine();
    CanvasTextureImageFactory::factory(engine)->notifyLoadedImages();

    // Call render in QML JavaScript side to queue the user's GL rendering commands.
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                         << " Emit paintGL() signal";

    emit paintGL();

    // Indicate texture completion point by queueing internalTextureComplete command
    m_renderer->commandQueue()->queueCommand(CanvasGlCommandQueue::internalTextureComplete);

    if (m_renderMode == RenderModeOffscreenBuffer) {
        // Trigger updatePaintNode() and actual frame draw
        update();
    }
    window()->update();
}

/*!
 * \internal
 */
void Canvas::queueResizeGL()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";

    m_resizeGLQueued = true;
}

/*!
 * \internal
 */
void Canvas::emitNeedRender()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";

    if (m_isNeedRenderQueued) {
        qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__
                                             << " needRender already queued, returning";
        return;
    }

    m_isNeedRenderQueued = true;
    emit needRender();
}

void Canvas::handleBeforeSynchronizing()
{
    qCDebug(canvas3drendering).nospace() << "Canvas3D::" << __FUNCTION__ << "()";

    updateWindowParameters();

    if (firstSync()) {
        m_rendererReady = true;
        return;
    }

    sync();
}

QT_CANVAS3D_END_NAMESPACE
QT_END_NAMESPACE
