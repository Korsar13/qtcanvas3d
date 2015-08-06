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

//
//  W A R N I N G
//  -------------
//
// This file is not part of the QtCanvas3D API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.

#ifndef CANVAS3D_P_H
#define CANVAS3D_P_H

#include "canvas3dcommon_p.h"
#include "context3d_p.h"

#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtGui/QOpenGLFramebufferObject>

QT_BEGIN_NAMESPACE

class QOffscreenSurface;

QT_CANVAS3D_BEGIN_NAMESPACE

class CanvasGlCommandQueue;
class CanvasRenderer;

// Logs on high level information about the OpenGL driver and context.
Q_DECLARE_LOGGING_CATEGORY(canvas3dinfo)

// Debug: logs all the calls made in to Canvas3D and Context3D
// Warning: debugs all warnings on failures in verifications
Q_DECLARE_LOGGING_CATEGORY(canvas3drendering)

// Debug: Logs all the OpenGL errors, this means calling glGetError()
// after each OpenGL call and this will cause a negative performance hit.
Q_DECLARE_LOGGING_CATEGORY(canvas3dglerrors)

class QT_CANVAS3D_EXPORT Canvas : public QQuickItem
{
    Q_OBJECT
    Q_DISABLE_COPY(Canvas)

    Q_ENUMS(RenderTarget)

    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(CanvasContext *context READ context NOTIFY contextChanged)
    Q_PROPERTY(float devicePixelRatio READ devicePixelRatio NOTIFY devicePixelRatioChanged)
    Q_PROPERTY(uint fps READ fps NOTIFY fpsChanged)
    Q_PROPERTY(QSize pixelSize READ pixelSize WRITE setPixelSize NOTIFY pixelSizeChanged)
    Q_PROPERTY(int width READ width WRITE setWidth NOTIFY widthChanged)
    Q_PROPERTY(int height READ height WRITE setHeight NOTIFY heightChanged)
    Q_PROPERTY(RenderTarget renderTarget READ renderTarget WRITE setRenderTarget NOTIFY renderTargetChanged REVISION 1)
    Q_PROPERTY(bool renderOnDemand READ renderOnDemand WRITE setRenderOnDemand NOTIFY renderOnDemandChanged REVISION 1)

public:
    enum RenderTarget {
        RenderTargetOffscreenBuffer,
        RenderTargetBackground,
        RenderTargetForeground
    };

    Canvas(QQuickItem *parent = 0);
    ~Canvas();

    void handleWindowChanged(QQuickWindow *win);
    float devicePixelRatio();
    QSize pixelSize();
    void setPixelSize(QSize pixelSize);
    void setWidth(int width);
    int width();
    void setHeight(int height);
    int height();
    void setRenderTarget(RenderTarget target);
    RenderTarget renderTarget() const;
    void setRenderOnDemand(bool enable);
    bool renderOnDemand() const;

    uint fps();
    Q_INVOKABLE int frameTimeMs();

    Q_INVOKABLE QJSValue getContext(const QString &name);
    Q_INVOKABLE QJSValue getContext(const QString &name, const QVariantMap &options);
    CanvasContext *context();
    CanvasRenderer *renderer();

public slots:
    void queueNextRender();
    void queueResizeGL();
    void requestRender();
    void emitNeedRender();
    void handleBeforeSynchronizing();
    void handleRendererFpsChange(uint fps);

signals:
    void needRender();
    void devicePixelRatioChanged(float ratio);
    void contextChanged(CanvasContext *context);
    void fpsChanged(uint fps);
    void pixelSizeChanged(QSize pixelSize);
    void widthChanged();
    void heightChanged();
    void renderTargetChanged();
    void renderOnDemandChanged();

    void initializeGL();
    void paintGL();
    void resizeGL(int width, int height, float devicePixelRatio);

    void textureReady(int id, const QSize &size, float devicePixelRatio);

protected:
    virtual void geometryChanged(const QRectF & newGeometry, const QRectF & oldGeometry);
    virtual void itemChange(ItemChange change, const ItemChangeData &value);
    virtual QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *);

private:
    void setupAntialiasing();
    void updateWindowParameters();
    void sync();
    bool firstSync();

    bool m_isNeedRenderQueued;
    bool m_rendererReady;
    CanvasContext *m_context3D;
    QSize m_fboSize;
    QSize m_maxSize;

    uint m_frameTimeMs;
    int m_maxSamples;
    float m_devicePixelRatio;

    bool m_isOpenGLES2;
    bool m_isSoftwareRendered;
    bool m_runningInDesigner;
    CanvasContextAttributes m_contextAttribs;
    bool m_isContextAttribsSet;
    bool m_resizeGLQueued;
    bool m_firstSync;
    RenderTarget m_renderTarget;
    bool m_renderOnDemand;

    CanvasRenderer *m_renderer;

    GLint m_maxVertexAttribs;
    int m_contextVersion;
    QSet<QByteArray> m_extensions;

    uint m_fps;
};

QT_CANVAS3D_END_NAMESPACE
QT_END_NAMESPACE

#endif // CANVAS3D_P_H

