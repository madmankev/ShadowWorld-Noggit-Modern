// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGITQT_ACTIONMANAGER_HPP
#define NOGGITQT_ACTIONMANAGER_HPP

#include <QObject>
#include <deque>
#include <stdexcept>
#include <noggit/Action.hpp>

class MapView;


namespace Noggit
{

    class ActionManager : public QObject
    {
    Q_OBJECT
    public:
        static ActionManager* instance()
        {
          static ActionManager inst;
          return &inst;
        }

        [[nodiscard]]
        std::deque<Action*>* getActionStack();

        [[nodiscard]]
        Action* getCurrentAction() const;

        void setCurrentAction(unsigned index);

        Action* beginAction(MapView* map_view
                            , int flags = ActionFlags::eNO_FLAG
                            , int modality_controls = ActionModalityControllers::eNONE);

        void endAction();

        //! Closes an action that can never reach endAction() because the code
        //! between beginAction() and endAction() bailed out (an exception, an
        //! access violation caught by the event dispatcher, ...).
        //!
        //! Without this the manager keeps a "current action" forever: beginAction()
        //! then returns the stale action, Tool::addMenuItem() callbacks start with
        //! `if (NOGGIT_CUR_ACTION) return;` and the object tool hotkeys are gated on
        //! `!NOGGIT_CUR_ACTION`, so copy/paste/delete/placement silently stop doing
        //! anything until the editor is restarted. endActionOnModalityMismatch()
        //! cannot recover those either: one-shot operations are begun without
        //! modality controllers and it returns early for them.
        void abandonCurrentAction(char const* reason);

        void endActionOnModalityMismatch(unsigned modality_controls);

        void setLimit(unsigned limit);

        void purge();

        [[nodiscard]]
        unsigned limit() const;

        void undo();
        void redo();

        ~ActionManager() override;

    signals:
      void popBack();
      void popFront();
      void addedAction(Action* action);
      void purged();
      void currentActionChanged(unsigned index);
      void onActionBegin(Action* action);
      void onActionEnd(Action* action);


    private:
        ActionManager() : QObject() {}

        std::deque<Action*> _action_stack;
        unsigned _limit = 30;
        Action* _cur_action = nullptr;
        unsigned _undo_index = 0;

    };

}

#define NOGGIT_CUR_ACTION Noggit::ActionManager::instance()->getCurrentAction()
#define NOGGIT_ACTION_MGR Noggit::ActionManager::instance()

namespace Noggit
{
    //! RAII wrapper around beginAction()/endAction().
    //!
    //! Every hand written begin/end pair in the editor is a chance to leave the
    //! manager holding a current action forever when the operation throws in the
    //! middle (see abandonCurrentAction()). Scope the action instead.
    class ActionScope
    {
    public:
        ActionScope(MapView* map_view
                    , int flags = ActionFlags::eNO_FLAG
                    , int modality_controls = ActionModalityControllers::eNONE)
            : _owns(!NOGGIT_ACTION_MGR->getCurrentAction())
        {
            _action = NOGGIT_ACTION_MGR->beginAction(map_view, flags, modality_controls);
        }

        ~ActionScope()
        {
            if (!_owns || NOGGIT_ACTION_MGR->getCurrentAction() != _action)
                return;

            try
            {
                NOGGIT_ACTION_MGR->endAction();
            }
            catch (...)
            {
                NOGGIT_ACTION_MGR->abandonCurrentAction("endAction() threw");
            }
        }

        ActionScope(ActionScope const&) = delete;
        ActionScope& operator=(ActionScope const&) = delete;

        [[nodiscard]] Action* action() const { return _action; }

    private:
        Action* _action = nullptr;
        bool _owns = false;
    };
}

#endif //NOGGITQT_ACTIONMANAGER_HPP

