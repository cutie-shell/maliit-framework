/* * This file is part of Maliit framework *
 *
 * Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
 * All rights reserved.
 *
 * Contact: maliit-discuss@lists.maliit.org
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * and appearing in the file LICENSE.LGPL included in the packaging
 * of this file.
 */

#include "minputmethodquick.h"

#include "mkeyoverridequick.h"
#include "minputmethodquickplugin.h"

#include <maliit/plugins/abstractinputmethodhost.h>
#include <maliit/plugins/abstractsurfacefactory.h>
#include <maliit/plugins/abstractwidgetssurface.h>
#include <maliit/plugins/keyoverride.h>

#include <QKeyEvent>
#include <QApplication>
#include <QDesktopWidget>
#include <QDebug>
#include <QRectF>
#include <QRect>
#include <QPainter>
#include <QPen>
#include <QBrush>
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
#include <QtQuick1/QDeclarativeComponent>
#include <QtQuick1/QDeclarativeContext>
#include <QtQuick1/QDeclarativeEngine>
#else
#include <QDeclarativeComponent>
#include <QDeclarativeContext>
#include <QDeclarativeEngine>
#endif
#include <QGraphicsTextItem>
#include <QGraphicsScene>
#include <QGraphicsObject>
#include <QDir>
#include <memory>

#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
#if defined(Q_WS_X11)
#include <QX11Info>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#endif
#endif

//this hack is needed because KeyPress definded by xlib conflicts with QEvent::KeyPress, breaking build
#undef KeyPress

namespace
{
    const char * const actionKeyName = "actionKey";

    const QRect &computeDisplayRect(QWidget *w = 0)
    {
        static const QRect displayRect(w ? qApp->desktop()->screenGeometry(w)
                                         : qApp->desktop()->screenGeometry());

        return displayRect;
    }
}

//! Helper class to load QML files and set up the declarative view accordingly.
class MInputMethodQuickLoader
{
private:
    QGraphicsScene *const m_scene;
    QDeclarativeEngine *const m_engine; //!< managed by controller
    std::auto_ptr<QDeclarativeComponent> m_component;
    QGraphicsObject *m_content; //!< managed by scene
    MInputMethodQuick *const m_controller;

public:
    MInputMethodQuickLoader(QGraphicsScene *newScene,
                            MInputMethodQuick *newController)
        : m_scene(newScene)
        , m_engine(new QDeclarativeEngine(newController))
        , m_content(0)
        , m_controller(newController)
    {
        Q_ASSERT(m_scene);
        Q_ASSERT(m_controller);

        m_engine->rootContext()->setContextProperty("MInputMethodQuick", m_controller);
        m_engine->addImportPath(MALIIT_PLUGINS_DATA_DIR);

        Q_FOREACH (const QString &path, MInputMethodQuickPlugin::qmlImportPaths()) {
            m_engine->addImportPath(path);
        }

        // Assuming that plugin B loads after plugin A, we need to make sure
        // that plugin B does not use the customized import paths of plugin A:
        MInputMethodQuickPlugin::setQmlImportPaths(QStringList());
    }

    virtual ~MInputMethodQuickLoader()
    {}

    // TODO: rename to showContent?
    void showUI()
    {
        if(not m_content) {
            qWarning() << __PRETTY_FUNCTION__
                       << "Content or controller missing: Cannot show UI.";
            return;
        }

        m_content->show();
    }

    void hideUI()
    {
        if(not m_content) {
            return;
        }

        m_content->hide();
    }

    void loadQmlFile(const QString &qmlFileName)
    {
        const bool wasContentVisible(m_content ? m_content->isVisible() : false);

        if (wasContentVisible) {
            m_controller->hide();
        }

        m_component.reset(new QDeclarativeComponent(m_engine, QUrl(qmlFileName)));

        if (not m_component->errors().isEmpty()) {
            qWarning() << "QML errors while loading " << qmlFileName << "\n"
                       << m_component->errors();
        }

        m_content = qobject_cast<QGraphicsObject *>(m_component->create());

        if (not m_content) {
            m_content = new QGraphicsTextItem("Error loading QML");
        }

        if (wasContentVisible) {
            m_controller->show();
            m_content->show();
        } else {
            m_content->hide();
        }

        m_scene->addItem(m_content);
    }

    QGraphicsObject *content() const
    {
        return m_content;
    }
};

class MInputMethodQuickPrivate
{
public:
    MInputMethodQuick *const q_ptr;
    QSharedPointer<Maliit::Plugins::AbstractGraphicsViewSurface> surface;
    QGraphicsScene *const scene;
    QGraphicsView *const view;
    MInputMethodQuickLoader *const loader;
    QRect inputMethodArea;
    int appOrientation;
    bool haveFocus;

    //! current active state
    Maliit::HandlerState activeState;

    //! In practice show() and hide() correspond to application SIP (close)
    //! requests.  We track the current shown/SIP requested state using these variables.
    bool sipRequested;
    bool sipIsInhibited;
    QSharedPointer<MKeyOverrideQuick> actionKeyOverride;
    QSharedPointer<MKeyOverride> sentActionKeyOverride;

    Q_DECLARE_PUBLIC(MInputMethodQuick);

    MInputMethodQuickPrivate(MAbstractInputMethodHost *host,
                             MInputMethodQuick *im)
        : q_ptr(im)
        , surface(qSharedPointerDynamicCast<Maliit::Plugins::AbstractGraphicsViewSurface>(host->surfaceFactory()->create(Maliit::Plugins::AbstractSurface::PositionCenterBottom | Maliit::Plugins::AbstractSurface::TypeGraphicsView)))
        , scene(surface->scene())
        , view(surface->view())
        , loader(new MInputMethodQuickLoader(scene, im))
        , appOrientation(0)
        , haveFocus(false)
        , activeState(Maliit::OnScreen)
        , sipRequested(false)
        , sipIsInhibited(false)
        , actionKeyOverride(new MKeyOverrideQuick())
        , sentActionKeyOverride()
    {
        updateActionKey(MKeyOverride::All);
        // Set surface size to fullscreen
        surface->setSize(QApplication::desktop()->screenGeometry().size());
    }

    ~MInputMethodQuickPrivate()
    {
        delete loader;
    }

    void handleInputMethodAreaUpdate(MAbstractInputMethodHost *host,
                                     const QRegion &region)
    {
        if (not host) {
            return;
        }
        host->setScreenRegion(region);
        host->setInputMethodArea(region);
    }

    void updateActionKey (const MKeyOverride::KeyOverrideAttributes changedAttributes)
    {
        actionKeyOverride->applyOverride(sentActionKeyOverride, changedAttributes);
    }
    
    void syncInputMask ()
    {
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
#if defined(Q_WS_X11)
        if (!view->effectiveWinId())
            return;

        const int size = 1;

        XRectangle * const rects = new XRectangle[size];

        quint32 customRegion[size * 4]; // custom region is pack of x, y, w, h
        rects[0].x = inputMethodArea.x();
        rects[0].y = inputMethodArea.y();
        rects[0].width = inputMethodArea.width();
        rects[0].height = inputMethodArea.height();
        customRegion[0] = inputMethodArea.x();
        customRegion[1] = inputMethodArea.y();
        customRegion[2] = inputMethodArea.width();
        customRegion[3] = inputMethodArea.height();


        const XserverRegion shapeRegion = XFixesCreateRegion(QX11Info::display(), rects, size);
        XFixesSetWindowShapeRegion(QX11Info::display(), view->effectiveWinId(), ShapeBounding, 0, 0, 0);
        XFixesSetWindowShapeRegion(QX11Info::display(), view->effectiveWinId(), ShapeInput, 0, 0, shapeRegion);

        XFixesDestroyRegion(QX11Info::display(), shapeRegion);

        XChangeProperty(QX11Info::display(), view->effectiveWinId(),
                        XInternAtom(QX11Info::display(), "_MEEGOTOUCH_CUSTOM_REGION", False),
                        XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *) customRegion, size * 4);

        delete[] rects;
#endif
#endif
    }
};

MInputMethodQuick::MInputMethodQuick(MAbstractInputMethodHost *host,
                                     const QString &qmlFileName)
    : MAbstractInputMethod(host)
    , d_ptr(new MInputMethodQuickPrivate(host, this))
{
    Q_D(MInputMethodQuick);

    d->loader->loadQmlFile(qmlFileName);
    
    propagateScreenSize();
}

MInputMethodQuick::~MInputMethodQuick()
{}

void MInputMethodQuick::handleFocusChange(bool focusIn)
{
    Q_D(MInputMethodQuick);
    d->haveFocus = focusIn;
}

void MInputMethodQuick::show()
{
    Q_D(MInputMethodQuick);
    d->sipRequested = true;
    if(d->sipIsInhibited) {
      return;
    }

    handleAppOrientationChanged(d->appOrientation);
    
    if (d->activeState == Maliit::OnScreen) {
      d->surface->show();
      d->loader->showUI();
      const QRegion r(inputMethodArea().toRect());
      d->handleInputMethodAreaUpdate(inputMethodHost(), r);
      d->syncInputMask();
    }
}

void MInputMethodQuick::hide()
{
    Q_D(MInputMethodQuick);
    if (!d->sipRequested) {
        return;
    }
    d->sipRequested = false;
    d->loader->hideUI();
    d->surface->hide();
    const QRegion r;
    d->handleInputMethodAreaUpdate(inputMethodHost(), r);
}

void MInputMethodQuick::handleAppOrientationChanged(int angle)
{
    Q_D(MInputMethodQuick);

    MAbstractInputMethod::handleAppOrientationChanged(angle);

    if (d->appOrientation != angle) {

        d->appOrientation = angle;
        // When emitted, QML Plugin will realice a state
        // change and update InputMethodArea. Don't propagate those changes except if
        // VkB is currently showed
        Q_EMIT appOrientationChanged(d->appOrientation);
        if (d->sipRequested && !d->sipIsInhibited) {
            d->handleInputMethodAreaUpdate(inputMethodHost(), inputMethodArea().toRect());
        }
    }
}

void MInputMethodQuick::setState(const QSet<Maliit::HandlerState> &state)
{
    Q_D(MInputMethodQuick);

    if (state.isEmpty()) {
        return;
    }

    if (state.contains(Maliit::OnScreen)) {
        d->activeState = Maliit::OnScreen;
        if (d->sipRequested && !d->sipIsInhibited) {
            show(); // Force reparent of client widgets.
        }
    } else {
        d->loader->hideUI();
        // Allow client to make use of InputMethodArea
        const QRegion r;
        d->handleInputMethodAreaUpdate(inputMethodHost(), r);
        d->activeState = *state.begin();
    }
}

void MInputMethodQuick::handleClientChange()
{
    Q_D(MInputMethodQuick);

    if (d->sipRequested) {
        d->loader->hideUI();
    }
}

void MInputMethodQuick::handleVisualizationPriorityChange(bool inhibitShow)
{
    Q_D(MInputMethodQuick);

    if (d->sipIsInhibited == inhibitShow) {
        return;
    }
    d->sipIsInhibited = inhibitShow;

    if (d->sipRequested) {
        if (inhibitShow) {
            d->loader->hideUI();
        } else {
            d->loader->showUI();
        }
    }
}


void MInputMethodQuick::propagateScreenSize()
{
    Q_EMIT screenWidthChanged(computeDisplayRect().width());
    Q_EMIT screenHeightChanged(computeDisplayRect().height());
}

int MInputMethodQuick::screenHeight() const
{
    return computeDisplayRect().height();
}

int MInputMethodQuick::screenWidth() const
{
    return computeDisplayRect().width();
}

int MInputMethodQuick::appOrientation() const
{
    Q_D(const MInputMethodQuick);
    return d->appOrientation;
}

QRectF MInputMethodQuick::inputMethodArea() const
{
    Q_D(const MInputMethodQuick);
    return d->inputMethodArea;
}

void MInputMethodQuick::setInputMethodArea(const QRectF &area)
{
    Q_D(MInputMethodQuick);

    if (d->inputMethodArea != area.toRect()) {
        d->inputMethodArea = area.toRect();

        qDebug() << __PRETTY_FUNCTION__ << "QWidget::effectiveWinId(): " << d_ptr->view->effectiveWinId();

        Q_EMIT inputMethodAreaChanged(d->inputMethodArea);
        d->syncInputMask();
    }
}

void MInputMethodQuick::sendPreedit(const QString &text)
{
    QList<Maliit::PreeditTextFormat> lst;
    inputMethodHost()->sendPreeditString(text, lst, text.length());
}

void MInputMethodQuick::sendKey(int key, int modifiers, const QString &text)
{
    QKeyEvent event(QEvent::KeyPress, key, (~(Qt::KeyboardModifiers(Qt::NoModifier))) & modifiers, text);
    inputMethodHost()->sendKeyEvent(event);
}

void MInputMethodQuick::sendCommit(const QString &text)
{
    if (text == "\b") {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        inputMethodHost()->sendKeyEvent(event);
    } else
    if ((text == "\r\n") || (text == "\n") || (text == "\r")) {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        inputMethodHost()->sendKeyEvent(event);
    } else {
        inputMethodHost()->sendCommitString(text);
    }
}

void MInputMethodQuick::pluginSwitchRequired(int switchDirection)
{
    inputMethodHost()->switchPlugin(
        static_cast<Maliit::SwitchDirection>(switchDirection));
}

void MInputMethodQuick::userHide()
{
    hide();
    inputMethodHost()->notifyImInitiatedHiding();
}

void MInputMethodQuick::setKeyOverrides(const QMap<QString, QSharedPointer<MKeyOverride> > &overrides)
{
    Q_D(MInputMethodQuick);
    const QMap<QString, QSharedPointer<MKeyOverride> >::const_iterator iter(overrides.find(actionKeyName));

    if (d->sentActionKeyOverride) {
        disconnect(d->sentActionKeyOverride.data(), SIGNAL(keyAttributesChanged(const QString &, const MKeyOverride::KeyOverrideAttributes)),
                   this, SLOT(onSentActionKeyAttributesChanged(const QString &, const MKeyOverride::KeyOverrideAttributes)));
        d->sentActionKeyOverride.clear();
    }

    if (iter != overrides.end()) {
        QSharedPointer<MKeyOverride> sentActionKeyOverride(*iter);

        if (sentActionKeyOverride) {
            d->sentActionKeyOverride = sentActionKeyOverride;
            connect(d->sentActionKeyOverride.data(), SIGNAL(keyAttributesChanged(const QString &, const MKeyOverride::KeyOverrideAttributes)),
                    this, SLOT(onSentActionKeyAttributesChanged(const QString &, const MKeyOverride::KeyOverrideAttributes)));
        }
    }
    d->updateActionKey(MKeyOverride::All);
}

QList<MAbstractInputMethod::MInputMethodSubView> MInputMethodQuick::subViews(Maliit::HandlerState state) const
{
    Q_UNUSED(state);
    MAbstractInputMethod::MInputMethodSubView sub_view;
    sub_view.subViewId = "";
    sub_view.subViewTitle = "";
    QList<MAbstractInputMethod::MInputMethodSubView> sub_views;
    sub_views << sub_view;
    return sub_views;
}

void MInputMethodQuick::onSentActionKeyAttributesChanged(const QString &, const MKeyOverride::KeyOverrideAttributes changedAttributes)
{
    Q_D(MInputMethodQuick);

    d->updateActionKey(changedAttributes);
}

MKeyOverrideQuick* MInputMethodQuick::actionKeyOverride() const
{
    Q_D(const MInputMethodQuick);

    return d->actionKeyOverride.data();
}

void MInputMethodQuick::activateActionKey()
{
    sendCommit("\n");
}
