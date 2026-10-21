// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_SAFE_APPLICATION_HPP
#define NOGGIT_SAFE_APPLICATION_HPP

#include <noggit/ActionManager.hpp>
#include <noggit/Log.h>

#include <QCoreApplication>
#include <QEvent>
#include <QMessageBox>
#include <QObject>
#include <QString>
#include <QtWidgets/QApplication>

#include <chrono>
#include <exception>
#include <string>

namespace Noggit::Application
{
  //! QApplication that does not let a C++ exception escape an event handler.
  //!
  //! Qt considers an exception leaving a slot undefined behaviour: on Windows it
  //! surfaces as an unhandled 0xE06D7363 exception and the editor dies with a
  //! stack trace instead of an error message. Noggit has many code paths that can
  //! legitimately throw while handling a click (a map that is not installed, a
  //! database file that cannot be read, a project without a client), so the
  //! exception is caught at the event dispatcher, written to the log and reported
  //! to the user.
  class SafeApplication : public QApplication
  {
  public:
    using QApplication::QApplication;

    bool notify(QObject* receiver, QEvent* event) override
    {
      try
      {
        return QApplication::notify(receiver, event);
      }
      catch (std::exception const& exception)
      {
        releaseInterruptedAction();
        report(receiver, event, exception.what());
      }
      catch (...)
      {
        releaseInterruptedAction();
        report(receiver, event, "unknown exception");
      }

      return false;
    }

  private:
    //! An exception escaping an event handler means whatever edit operation the
    //! handler started will never be ended. Left alone the ActionManager keeps a
    //! "current action" forever and the editor silently stops accepting object
    //! placement, copy, paste and delete (their hotkeys and menu items are gated
    //! on there being no action in progress).
    static void releaseInterruptedAction()
    {
      try
      {
        Noggit::ActionManager::instance()->abandonCurrentAction("an event handler threw");
      }
      catch (...)
      {
      }
    }

    //! Named by hand: QEvent::Type is not a reflected enum in every Qt version
    //! this fork builds against.
    static char const* eventName(QEvent const* event)
    {
      if (!event)
        return "unknown";

      switch (event->type())
      {
      case QEvent::MouseButtonPress: return "MouseButtonPress";
      case QEvent::MouseButtonRelease: return "MouseButtonRelease";
      case QEvent::MouseButtonDblClick: return "MouseButtonDblClick";
      case QEvent::MouseMove: return "MouseMove";
      case QEvent::Wheel: return "Wheel";
      case QEvent::KeyPress: return "KeyPress";
      case QEvent::KeyRelease: return "KeyRelease";
      case QEvent::Paint: return "Paint";
      case QEvent::Resize: return "Resize";
      case QEvent::Move: return "Move";
      case QEvent::Show: return "Show";
      case QEvent::Hide: return "Hide";
      case QEvent::Close: return "Close";
      case QEvent::Timer: return "Timer";
      case QEvent::FocusIn: return "FocusIn";
      case QEvent::FocusOut: return "FocusOut";
      case QEvent::Enter: return "Enter";
      case QEvent::Leave: return "Leave";
      case QEvent::ActivationChange: return "ActivationChange";
      case QEvent::WindowActivate: return "WindowActivate";
      case QEvent::DeferredDelete: return "DeferredDelete";
      case QEvent::ContextMenu: return "ContextMenu";
      default: return nullptr;
      }
    }

    static std::string describe(QObject* receiver, QEvent* event)
    {
      std::string description = "a ";

      if (auto const* name = eventName(event))
        description += name;
      else
        description += "type " + std::to_string(event ? static_cast<int>(event->type()) : -1);

      description += " event for ";
      description += (receiver && receiver->metaObject()) ? receiver->metaObject()->className()
                                                          : "an unknown object";

      return description;
    }

    static void report(QObject* receiver, QEvent* event, char const* what)
    {
      auto const description = describe(receiver, event);

      LogError << "Uncaught exception while handling " << description << ": " << what << std::endl;

      // Dialogs are rate limited: a paint or timer handler that throws on every
      // pass must not be able to flood the user with modal windows.
      static int shown = 0;
      static auto last_shown = std::chrono::steady_clock::time_point{};

      auto const now = std::chrono::steady_clock::now();
      auto const since_last = std::chrono::duration_cast<std::chrono::seconds>(now - last_shown).count();

      if (shown >= 5 || (last_shown.time_since_epoch().count() != 0 && since_last < 3))
        return;

      ++shown;
      last_shown = now;

      QMessageBox::critical(nullptr, "Noggit",
                            QString("Noggit could not finish that operation (%1):\n\n%2")
                              .arg(QString::fromStdString(description), QString::fromUtf8(what)));
    }
  };
}

#endif //NOGGIT_SAFE_APPLICATION_HPP
