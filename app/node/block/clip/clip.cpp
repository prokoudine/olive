/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2022 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "clip.h"

#include "node/output/track/track.h"
#include "node/output/viewer/viewer.h"
#include "widget/slider/floatslider.h"
#include "widget/slider/rationalslider.h"

namespace olive {

#define super Block

const QString ClipBlock::kBufferIn = QStringLiteral("buffer_in");
const QString ClipBlock::kMediaInInput = QStringLiteral("media_in_in");
const QString ClipBlock::kSpeedInput = QStringLiteral("speed_in");
const QString ClipBlock::kReverseInput = QStringLiteral("reverse_in");
const QString ClipBlock::kMaintainAudioPitchInput = QStringLiteral("maintain_audio_pitch_in");
const QString ClipBlock::kAutoCacheInput = QStringLiteral("autocache_in");

ClipBlock::ClipBlock() :
  in_transition_(nullptr),
  out_transition_(nullptr),
  connected_viewer_(nullptr)
{
  AddInput(kMediaInInput, NodeValue::kRational, InputFlags(kInputFlagNotConnectable | kInputFlagNotKeyframable));
  SetInputProperty(kMediaInInput, QStringLiteral("view"), RationalSlider::kTime);
  SetInputProperty(kMediaInInput, QStringLiteral("viewlock"), true);

  AddInput(kSpeedInput, NodeValue::kFloat, 1.0, InputFlags(kInputFlagNotConnectable | kInputFlagNotKeyframable));
  SetInputProperty(kSpeedInput, QStringLiteral("view"), FloatSlider::kPercentage);
  SetInputProperty(kSpeedInput, QStringLiteral("min"), 0.0);

  AddInput(kReverseInput, NodeValue::kBoolean, false, InputFlags(kInputFlagNotConnectable | kInputFlagNotKeyframable));

  AddInput(kMaintainAudioPitchInput, NodeValue::kBoolean, false, InputFlags(kInputFlagNotConnectable | kInputFlagNotKeyframable));

  AddInput(kAutoCacheInput, NodeValue::kBoolean, false, InputFlags(kInputFlagNotConnectable | kInputFlagNotKeyframable));

  PrependInput(kBufferIn, NodeValue::kNone, InputFlags(kInputFlagNotKeyframable));
  //SetValueHintForInput(kBufferIn, ValueHint(NodeValue::kBuffer));

  SetEffectInput(kBufferIn);
}

QString ClipBlock::Name() const
{
  if (track()) {
    if (track()->type() == Track::kVideo) {
      return tr("Video Clip");
    } else if (track()->type() == Track::kAudio) {
      return tr("Audio Clip");
    }
  }

  return tr("Clip");
}

QString ClipBlock::id() const
{
  return QStringLiteral("org.olivevideoeditor.Olive.clip");
}

QString ClipBlock::Description() const
{
  return tr("A time-based node that represents a media source.");
}

void ClipBlock::set_length_and_media_out(const rational &length)
{
  if (length == this->length()) {
    return;
  }

  if (reverse()) {
    // Calculate media_in adjustment
    rational proposed_media_in = SequenceToMediaTime(this->length() - length, true);
    set_media_in(proposed_media_in);
  }

  super::set_length_and_media_out(length);
}

void ClipBlock::set_length_and_media_in(const rational &length)
{
  if (length == this->length()) {
    return;
  }

  if (!reverse()) {
    // Calculate media_in adjustment
    rational proposed_media_in = SequenceToMediaTime(this->length() - length, false, true);
    set_media_in(proposed_media_in);
  }

  super::set_length_and_media_in(length);
}

rational ClipBlock::media_in() const
{
  return GetStandardValue(kMediaInInput).value<rational>();
}

void ClipBlock::set_media_in(const rational &media_in)
{
  SetStandardValue(kMediaInInput, QVariant::fromValue(media_in));
}

void ClipBlock::SetAutocache(bool e)
{
  SetStandardValue(kAutoCacheInput, e);
}

rational ClipBlock::SequenceToMediaTime(const rational &sequence_time, bool ignore_reverse, bool ignore_speed) const
{
  // These constants are not considered "values" per se, so we don't modify them
  if (sequence_time == RATIONAL_MIN || sequence_time == RATIONAL_MAX) {
    return sequence_time;
  }

  rational media_time = sequence_time;

  if (reverse() && !ignore_reverse) {
    media_time = length() - media_time;
  }

  if (!ignore_speed) {
    double speed_value = speed();
    if (qIsNull(speed_value)) {
      // Effectively holds the frame at the in point
      media_time = 0;
    } else if (!qFuzzyCompare(speed_value, 1.0)) {
      // Multiply time
      media_time = rational::fromDouble(media_time.toDouble() * speed_value);
    }
  }

  media_time += media_in();

  return media_time;
}

rational ClipBlock::MediaToSequenceTime(const rational &media_time) const
{
  // These constants are not considered "values" per se, so we don't modify them
  if (media_time == RATIONAL_MIN || media_time == RATIONAL_MAX) {
    return media_time;
  }

  rational sequence_time = media_time - media_in();

  double speed_value = speed();
  if (qIsNull(speed_value)) {
    // I don't know what to return here yet...
    sequence_time = rational::NaN;
  } else if (!qFuzzyCompare(speed_value, 1.0)) {
    // Divide time
    sequence_time = rational::fromDouble(sequence_time.toDouble() / speed_value);
  }

  if (reverse()) {
    sequence_time = length() - sequence_time;
  }

  return sequence_time;
}

void ClipBlock::RequestRangeFromConnected(const TimeRange &range)
{
  Track::Type type = GetTrackType();

  if (type == Track::kVideo || type == Track::kAudio) {
    if (Node *connected = GetConnectedOutput(kBufferIn)) {
      TimeRange max_range = InputTimeAdjustment(kBufferIn, -1, TimeRange(0, length()));
      if (type == Track::kVideo) {
        // Handle thumbnails
        RequestRangeForCache(connected->thumbnail_cache(), max_range, range, true, true);

        // Handle video cache
        RequestRangeForCache(connected->video_frame_cache(), max_range, range, true, IsAutocaching());
      } else if (type == Track::kAudio) {
        // Handle waveforms
        RequestRangeForCache(connected->waveform_cache(), max_range, range, true, true);

        // Handle audio cache
        RequestRangeForCache(connected->audio_playback_cache(), max_range, range, true, IsAutocaching());
      }
    }
  }
}

void ClipBlock::RequestInvalidatedFromConnected()
{
  Track::Type type = GetTrackType();

  if (type == Track::kVideo || type == Track::kAudio) {
    if (Node *connected = GetConnectedOutput(kBufferIn)) {
      TimeRange max_range = InputTimeAdjustment(kBufferIn, -1, TimeRange(0, length()));
      if (type == Track::kVideo) {
        // Handle thumbnails
        RequestInvalidatedForCache(connected->thumbnail_cache(), max_range);

        // Handle video cache
        if (IsAutocaching()) {
          RequestInvalidatedForCache(connected->video_frame_cache(), max_range);
        }
      } else if (type == Track::kAudio) {
        // Handle waveforms
        RequestInvalidatedForCache(connected->waveform_cache(), max_range);

        // Handle audio cache
        if (IsAutocaching()) {
          RequestInvalidatedForCache(connected->audio_playback_cache(), max_range);
        }
      }
    }
  }
}

void ClipBlock::RequestRangeForCache(PlaybackCache *cache, const TimeRange &max_range, const TimeRange &range, bool invalidate, bool request)
{
  TimeRange r = range.Intersected(max_range);

  if (invalidate) {
    cache->Invalidate(r);
  }

  if (request) {
    emit cache->Request(r);
  }
}

void ClipBlock::RequestInvalidatedForCache(PlaybackCache *cache, const TimeRange &max_range)
{
  TimeRangeList invalid = cache->GetInvalidatedRanges(max_range);

  for (const PlaybackCache::Passthrough &p : cache->GetPassthroughs()) {
    invalid.remove(p);
  }

  for (const TimeRange &r : invalid) {
    RequestRangeForCache(cache, max_range, r, false, true);
  }
}

void ClipBlock::InvalidateCache(const TimeRange& range, const QString& from, int element, InvalidateCacheOptions options)
{
  Q_UNUSED(element)

  // If signal is from texture input, transform all times from media time to sequence time
  if (from == kBufferIn) {
    // Render caches where necessary
    if (AreCachesEnabled()) {
      RequestRangeFromConnected(range);
    }

    // Adjust range from media time to sequence time
    TimeRange adj;
    double speed_value = speed();

    if (qIsNull(speed_value)) {
      // Handle 0 speed by invalidating the whole clip
      adj = TimeRange(RATIONAL_MIN, RATIONAL_MAX);
    } else {
      adj = TimeRange(MediaToSequenceTime(range.in()), MediaToSequenceTime(range.out()));
    }

    // Find connected viewer node
    auto viewers = FindInputNodesConnectedToInput<ViewerOutput>(NodeInput(this, kBufferIn));
    ViewerOutput *new_connected_viewer = viewers.isEmpty() ? nullptr : viewers.first();

    if (new_connected_viewer != connected_viewer_) {
      if (connected_viewer_) {
        disconnect(connected_viewer_->GetTimelinePoints()->markers(), &TimelineMarkerList::MarkerAdded, this, &ClipBlock::PreviewChanged);
        disconnect(connected_viewer_->GetTimelinePoints()->markers(), &TimelineMarkerList::MarkerRemoved, this, &ClipBlock::PreviewChanged);
        disconnect(connected_viewer_->GetTimelinePoints()->markers(), &TimelineMarkerList::MarkerModified, this, &ClipBlock::PreviewChanged);
      }

      connected_viewer_ = new_connected_viewer;

      if (connected_viewer_) {
        connect(connected_viewer_->GetTimelinePoints()->markers(), &TimelineMarkerList::MarkerAdded, this, &ClipBlock::PreviewChanged);
        connect(connected_viewer_->GetTimelinePoints()->markers(), &TimelineMarkerList::MarkerRemoved, this, &ClipBlock::PreviewChanged);
        connect(connected_viewer_->GetTimelinePoints()->markers(), &TimelineMarkerList::MarkerModified, this, &ClipBlock::PreviewChanged);
      }
    }

    super::InvalidateCache(adj, from, element, options);
  } else {
    // Otherwise, pass signal along normally
    super::InvalidateCache(range, from, element, options);
  }
}

void ClipBlock::LinkChangeEvent()
{
  block_links_.clear();

  foreach (Node* n, links()) {
    ClipBlock* b = dynamic_cast<ClipBlock*>(n);

    if (b) {
      block_links_.append(b);
    }
  }
}

void ClipBlock::InputConnectedEvent(const QString &input, int element, Node *output)
{
  super::InputConnectedEvent(input, element, output);

  if (input == kBufferIn) {
    connect(output->thumbnail_cache(), &FrameHashCache::Validated, this, &Block::PreviewChanged);
    connect(output->waveform_cache(), &AudioPlaybackCache::Validated, this, &Block::PreviewChanged);
    connect(output->video_frame_cache(), &FrameHashCache::Validated, this, &Block::PreviewChanged);
    connect(output->audio_playback_cache(), &AudioPlaybackCache::Validated, this, &Block::PreviewChanged);
  }
}

void ClipBlock::InputDisconnectedEvent(const QString &input, int element, Node *output)
{
  super::InputDisconnectedEvent(input, element, output);

  if (input == kBufferIn) {
    disconnect(output->thumbnail_cache(), &FrameHashCache::Validated, this, &Block::PreviewChanged);
    disconnect(output->waveform_cache(), &AudioPlaybackCache::Validated, this, &Block::PreviewChanged);
    disconnect(output->video_frame_cache(), &FrameHashCache::Validated, this, &Block::PreviewChanged);
    disconnect(output->audio_playback_cache(), &AudioPlaybackCache::Validated, this, &Block::PreviewChanged);
  }
}

void ClipBlock::InputValueChangedEvent(const QString &input, int element)
{
  super::InputValueChangedEvent(input, element);

  if (input == kAutoCacheInput) {
    if (IsAutocaching()) {
      RequestInvalidatedFromConnected();
    } else {
      Track::Type type = GetTrackType();

      if (Node *connected = GetConnectedOutput(kBufferIn)) {
        if (type == Track::kVideo) {
          emit connected->video_frame_cache()->CancelAll();
        } else if (type == Track::kAudio) {
          emit connected->audio_playback_cache()->CancelAll();
        }
      }
    }
  }
}

TimeRange ClipBlock::InputTimeAdjustment(const QString& input, int element, const TimeRange& input_time) const
{
  Q_UNUSED(element)

  if (input == kBufferIn) {
    return TimeRange(SequenceToMediaTime(input_time.in()), SequenceToMediaTime(input_time.out()));
  }

  return super::InputTimeAdjustment(input, element, input_time);
}

TimeRange ClipBlock::OutputTimeAdjustment(const QString& input, int element, const TimeRange& input_time) const
{
  Q_UNUSED(element)

  if (input == kBufferIn) {
    return TimeRange(MediaToSequenceTime(input_time.in()), MediaToSequenceTime(input_time.out()));
  }

  return super::OutputTimeAdjustment(input, element, input_time);
}

void ClipBlock::Value(const NodeValueRow &value, const NodeGlobals &globals, NodeValueTable *table) const
{
  Q_UNUSED(globals)

  // We discard most values here except for the buffer we received
  NodeValue data = value[kBufferIn];

  table->Clear();
  if (data.type() != NodeValue::kNone) {
    table->Push(data);
  }
}

void ClipBlock::Retranslate()
{
  super::Retranslate();

  SetInputName(kBufferIn, tr("Buffer"));
  SetInputName(kMediaInInput, tr("Media In"));
  SetInputName(kSpeedInput, tr("Speed"));
  SetInputName(kReverseInput, tr("Reverse"));
  SetInputName(kMaintainAudioPitchInput, tr("Maintain Audio Pitch"));
}

void ClipBlock::AddCachePassthroughFrom(ClipBlock *other)
{
  if (auto tc = this->video_frame_cache()) {
    if (auto oc = other->video_frame_cache()) {
      tc->SetPassthrough(oc);
    }
  }

  if (auto tc = this->audio_playback_cache()) {
    if (auto oc = other->audio_playback_cache()) {
      tc->SetPassthrough(oc);
    }
  }

  if (auto tc = this->thumbnails()) {
    if (auto oc = other->thumbnails()) {
      tc->SetPassthrough(oc);
    }
  }

  if (auto tc = this->waveform()) {
    if (auto oc = other->waveform()) {
      tc->SetPassthrough(oc);
    }
  }
}

void ClipBlock::ConnectedToPreviewEvent()
{
  RequestInvalidatedFromConnected();
}

}
