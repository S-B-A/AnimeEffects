#include <QPainter>
#include "util/TreeIterator.h"
#include "cmnd/ScopedMacro.h"
#include "ctrl/TimeLineEditor.h"
#include "ctrl/TimeLineRenderer.h"

using namespace core;

namespace
{

static const int kTimeLineFpsA = 60;
static const int kTimeLineFpsB = 30;
static const int kTimeLineFpsC = 10;
static const int kTimeLineMargin = 14;
static const int kHeaderHeight = 22;
static const int kDefaultMaxFrame = 600;

}

namespace ctrl
{

//-------------------------------------------------------------------------------------------------
TimeLineEditor::TimeCurrent::TimeCurrent()
    : mMaxFrame(0)
    , mFrame(0)
    , mPos()
{
    mPos.setY(11);
}

void TimeLineEditor::TimeCurrent::setMaxFrame(int aMaxFrame)
{
    mMaxFrame = aMaxFrame;
}

void TimeLineEditor::TimeCurrent::setFrame(const TimeLineScale& aScale, core::Frame aFrame)
{
    mFrame = aFrame;
    mFrame.clamp(0, mMaxFrame);
    mPos.setX(kTimeLineMargin + aScale.pixelWidth(mFrame.get()));
}

void TimeLineEditor::TimeCurrent::setHandlePos(const TimeLineScale& aScale, const QPoint& aPos)
{
    mFrame.set(aScale.frame(aPos.x() - kTimeLineMargin));
    mFrame.clamp(0, mMaxFrame);
    mPos.setX(kTimeLineMargin + aScale.pixelWidth(mFrame.get()));
}

void TimeLineEditor::TimeCurrent::update(const TimeLineScale& aScale)
{
    mPos.setX(kTimeLineMargin + aScale.pixelWidth(mFrame.get()));
}


//-------------------------------------------------------------------------------------------------
TimeLineEditor::TimeLineEditor()
    : mProject()
    , mRows()
    , mSelectingRow()
    , mTimeMax()
    , mState(State_Standby)
    , mTimeCurrent()
    , mTimeScale()
    , mFocus(mRows, mTimeScale, kTimeLineMargin)
    , mMoveRef()
    , mMoveFrame()
    , mOnUpdatingKey(false)
    , mShowSelectionRange(false)
{
    mRows.reserve(64);

    const std::array<int, 3> kFrameList =
    {
        kTimeLineFpsA,
        kTimeLineFpsB,
        kTimeLineFpsC
    };
    mTimeScale.setFrameList(kFrameList);

    // reset max frame
    setMaxFrame(kDefaultMaxFrame);
}

void TimeLineEditor::setMaxFrame(int aValue)
{
    mTimeMax = aValue;
    mTimeScale.setMaxFrame(mTimeMax);
    mTimeCurrent.setMaxFrame(mTimeMax);
    mTimeCurrent.setFrame(mTimeScale, core::Frame(0));
}

void TimeLineEditor::setProject(Project* aProject)
{
    clearRows();
    mProject.reset();

    if (aProject)
    {
        mProject = aProject->pointee();
        setMaxFrame(mProject->attribute().maxFrame());
    }
    else
    {
        setMaxFrame(kDefaultMaxFrame);
    }
}

void TimeLineEditor::clearRows()
{
    mRows.clear();
    clearState();
}

void TimeLineEditor::clearState()
{
    mFocus.clear();
    mState = State_Standby;
    mMoveRef = NULL;
    mMoveFrame = 0;
    mShowSelectionRange = false;
}

void TimeLineEditor::pushRow(ObjectNode* aNode, util::Range aWorldTB, bool aClosedFolder)
{
    const int left = kTimeLineMargin;
    const int right = left + mTimeScale.maxPixelWidth();
    const QRect rect(QPoint(left, aWorldTB.min()), QPoint(right, aWorldTB.max()));
    TimeLineRow row = { aNode, rect, aClosedFolder, aNode == mSelectingRow };
    mRows.push_back(row);
}

void TimeLineEditor::updateRowSelection(const core::ObjectNode* aRepresent)
{
    mSelectingRow = aRepresent;
    for (auto& row : mRows)
    {
        row.selecting = (row.node && row.node == aRepresent);
    }
}

void TimeLineEditor::updateKey()
{
    if (!mOnUpdatingKey)
    {
        clearState();
    }
}

void TimeLineEditor::updateProjectAttribute()
{
    clearState();
    if (mProject)
    {
        const int newMaxFrame = mProject->attribute().maxFrame();
        if (mTimeMax != newMaxFrame)
        {
            setMaxFrame(newMaxFrame);

            const int newRowRight = kTimeLineMargin + mTimeScale.maxPixelWidth();
            for (auto& row : mRows)
            {
                row.rect.setRight(newRowRight);
            }
        }
    }
}

TimeLineEditor::UpdateFlags TimeLineEditor::updateCursor(const AbstractCursor& aCursor)
{
    TimeLineEditor::UpdateFlags flags = 0;

    if (!mProject)
    {
        return flags;
    }

    const QPoint worldPoint = aCursor.worldPoint();

    if (aCursor.isLeftPressState())
    {
        // a selection range is exists.
        if (mState == State_EncloseKeys)
        {
            if (mFocus.isInRange(worldPoint) && beginMoveKeys(worldPoint))
            {
                mState = State_MoveKeys;
                flags |= UpdateFlag_ModView;
            }
            else
            {
                mShowSelectionRange = false;
                mState = State_Standby;
                flags |= UpdateFlag_ModView;
            }
        }

        // idle state
        if (mState == State_Standby)
        {
            const auto target = mFocus.reset(worldPoint);

            const QVector2D handlePos(mTimeCurrent.handlePos());

            if ((aCursor.screenPos() - handlePos).length() < mTimeCurrent.handleRange())
            {
                mState = State_MoveCurrent;
                flags |= UpdateFlag_ModView;
            }
            else if (aCursor.screenPos().y() < kHeaderHeight)
            {
                mTimeCurrent.setHandlePos(mTimeScale, aCursor.worldPos().toPoint());
                mState = State_MoveCurrent;
                flags |= UpdateFlag_ModView;
                flags |= UpdateFlag_ModFrame;
            }
            else if (target.isValid())
            {
                beginMoveKey(target);
                mState = State_MoveKeys;
                flags |= UpdateFlag_ModView;
            }
            else
            {
                mShowSelectionRange = true;
                mState = State_EncloseKeys;
                flags |= UpdateFlag_ModView;
            }
        }
    }
    else if (aCursor.isLeftMoveState())
    {
        if (mState == State_MoveCurrent)
        {
            mTimeCurrent.setHandlePos(mTimeScale, aCursor.worldPos().toPoint());
            flags |= UpdateFlag_ModView;
            flags |= UpdateFlag_ModFrame;
        }
        else if (mState == State_MoveKeys)
        {
            if (!modifyMoveKeys(aCursor.worldPoint()))
            {
                mState = State_Standby;
                mMoveRef = NULL;
                mFocus.clear();
            }
            flags |= UpdateFlag_ModView;
            flags |= UpdateFlag_ModFrame;
        }
        else if (mState == State_EncloseKeys)
        {
            mFocus.update(aCursor.worldPoint());
            flags |= UpdateFlag_ModView;
        }
    }
    else if (aCursor.isLeftReleaseState())
    {
        if (mState != State_EncloseKeys || !mFocus.hasRange())
        {
            mMoveRef = NULL;
            mState = State_Standby;
            mShowSelectionRange = false;
            flags |= UpdateFlag_ModView;
        }
    }
    else
    {
        if (mState != State_EncloseKeys)
        {
            mFocus.reset(aCursor.worldPoint());
        }
    }

    if (mFocus.viewIsChanged())
    {
        flags |= UpdateFlag_ModView;
    }

    return flags;
}

void TimeLineEditor::beginMoveKey(const TimeLineFocus::SingleFocus& aTarget)
{
    XC_ASSERT(aTarget.isValid());

    mOnUpdatingKey = true;
    {
        cmnd::ScopedMacro macro(mProject->commandStack(), "move time key");

        auto notifier = TimeLineUtil::createMoveNotifier(
                            *mProject, *aTarget.node, aTarget.pos);
        macro.grabListener(notifier);

        mMoveRef = new TimeLineUtil::MoveKey(notifier->event());
        mProject->commandStack().push(mMoveRef);
        mMoveFrame = aTarget.pos.index();
    }
    mOnUpdatingKey = false;
}

bool TimeLineEditor::beginMoveKeys(const QPoint& aWorldPos)
{
    bool success = false;
    mOnUpdatingKey = true;
    {
        auto notifier = new TimeLineUtil::Notifier(*mProject);
        notifier->event().setType(TimeLineEvent::Type_MoveKey);

        if (mFocus.select(notifier->event()))
        {
            cmnd::ScopedMacro macro(mProject->commandStack(), "move time keys");

            macro.grabListener(notifier);
            mMoveRef = new TimeLineUtil::MoveKey(notifier->event());
            mProject->commandStack().push(mMoveRef);
            mMoveFrame = mTimeScale.frame(aWorldPos.x() - kTimeLineMargin);
            success = true;
        }
        else
        {
            delete notifier;
            mMoveRef = NULL;
        }
    }
    mOnUpdatingKey = false;
    return success;
}

bool TimeLineEditor::modifyMoveKeys(const QPoint& aWorldPos)
{
    if (mProject->commandStack().isModifiable(mMoveRef))
    {
        const int newFrame = mTimeScale.frame(aWorldPos.x() - kTimeLineMargin);
        const int addFrame = newFrame - mMoveFrame;
        TimeLineEvent modEvent;

        mOnUpdatingKey = true;
        if (mMoveRef->modifyMove(modEvent, addFrame, util::Range(0, mTimeMax)))
        {
            mMoveFrame = newFrame;
            mFocus.moveBoundingRect(addFrame);
            mProject->onTimeLineModified(modEvent, false);
        }
        mOnUpdatingKey = false;
        return true;
    }
    return false;
}

bool TimeLineEditor::checkDeletableKeys(core::TimeLineEvent& aEvent, const QPoint& aPos)
{
    if (mFocus.hasRange() && !mFocus.isInRange(aPos))
    {
        return false;
    }

    return mFocus.select(aEvent);
}

void TimeLineEditor::deleteCheckedKeys(core::TimeLineEvent& aEvent)
{
    XC_ASSERT(!aEvent.targets().isEmpty());

    mOnUpdatingKey = true;
    {
        cmnd::Stack& stack = mProject->commandStack();

        // create notifier
        auto notifier = new TimeLineUtil::Notifier(*mProject);
        notifier->event() = aEvent;
        notifier->event().setType(core::TimeLineEvent::Type_RemoveKey);

        // push delete keys command
        cmnd::ScopedMacro macro(stack, "remove time keys");
        macro.grabListener(notifier);

        for (auto target : aEvent.targets())
        {
            core::TimeLine* line = target.pos.line();
            XC_PTR_ASSERT(line);
            stack.push(line->createRemover(target.pos.type(), target.pos.index(), true));
        }
    }
    mOnUpdatingKey = false;

    clearState();
}

void TimeLineEditor::updateWheel(int aDelta)
{
    mTimeScale.update(aDelta);
    mTimeCurrent.update(mTimeScale);

    const int lineWidth = mTimeScale.maxPixelWidth();

    for (TimeLineRow& row : mRows)
    {
        row.rect.setWidth(lineWidth);
    }
}

void TimeLineEditor::setFrame(core::Frame aFrame)
{
    mTimeCurrent.setFrame(mTimeScale, aFrame);
}

core::Frame TimeLineEditor::currentFrame() const
{
    return mTimeCurrent.frame();
}

QSize TimeLineEditor::modelSpaceSize() const
{
    int height = kHeaderHeight + 1 + 64; // with margin

    if (!mRows.empty())
    {
        height += mRows.back().rect.bottom() - mRows.front().rect.top();
    }

    const int width = mTimeScale.maxPixelWidth() + 2 * kTimeLineMargin;

    return QSize(width, height);
}

QPoint TimeLineEditor::currentTimeCursorPos() const
{
    return mTimeCurrent.handlePos();
}

void TimeLineEditor::render(QPainter& aPainter, const CameraInfo& aCamera, const QRect& aCullRect)
{
    if (aCamera.screenWidth() < 2 * kTimeLineMargin) return;

    const QRect camRect(-aCamera.pos().toPoint(), aCamera.screenSize());
    const QRect cullRect(aCullRect.marginsAdded(QMargins(2, 2, 2, 2))); // use culling

    const int margin = kTimeLineMargin;
    const int bgn = mTimeScale.frame(cullRect.left() - margin - 5);
    const int end = mTimeScale.frame(cullRect.right() - margin + 5);

    TimeLineRenderer renderer(aPainter, aCamera);
    renderer.setMargin(margin);
    renderer.setRange(util::Range(bgn, end));
    renderer.setTimeScale(mTimeScale);

    renderer.renderLines(mRows, camRect, cullRect);
    renderer.renderHeader(kHeaderHeight, kTimeLineFpsA);
    //renderer.renderHandle(mTimeCurrent.handlePos(), mTimeCurrent.handleRange());

    if (mShowSelectionRange)
    {
        renderer.renderSelectionRange(mFocus.visualRect());
    }
}

} // namespace ctrl