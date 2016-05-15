/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Navico BR24 Radar Plugin
 * Author:   David Register
 *           Dave Cowell
 *           Kees Verruijt
 *           Douwe Fokkema
 *           Sean D'Epagnier
 ***************************************************************************
 *   Copyright (C) 2010 by David S. Register              bdbcat@yahoo.com *
 *   Copyright (C) 2012-2013 by Dave Cowell                                *
 *   Copyright (C) 2012-2016 by Kees Verruijt         canboat@verruijt.net *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************
 */

#include "RadarInfo.h"
#include "drawutil.h"
#include "br24Receive.h"
#include "br24Transmit.h"
#include "RadarDraw.h"
#include "RadarCanvas.h"
#include "RadarPanel.h"

PLUGIN_BEGIN_NAMESPACE

enum { TIMER_ID = 1 };

BEGIN_EVENT_TABLE(RadarInfo, wxEvtHandler)
EVT_TIMER(TIMER_ID, RadarInfo::RefreshDisplay)
END_EVENT_TABLE()

void radar_control_item::Update(int v) {
  wxMutexLocker lock(m_mutex);

  if (v != button) {
    mod = true;
    button = v;
  }
  value = v;
};

RadarInfo::RadarInfo(br24radar_pi *pi, wxString name, int radar) {
  this->m_pi = pi;
  this->name = name;
  this->radar = radar;

  this->radar_type = RT_UNKNOWN;
  this->auto_range_mode = true;

  this->transmit = new br24Transmit(name, radar);
  this->receive = 0;
  this->m_draw_panel.draw = 0;
  this->m_draw_overlay.draw = 0;
  this->radar_panel = 0;
  this->control_dialog = 0;

  for (size_t z = 0; z < GUARD_ZONES; z++) {
    guard_zone[z] = new GuardZone(pi);
  }

  m_timer = new wxTimer(this, TIMER_ID);
  m_refreshes_queued = 0;
  m_refresh_millis = 1000;
  m_timer->Start(m_refresh_millis);

  m_quit = false;
}

RadarInfo::~RadarInfo() {
  wxMutexLocker lock(m_mutex);

  m_quit = true;

  m_timer->Stop();

  delete transmit;
  if (receive) {
    receive->Wait();
    delete receive;
  }
  if (radar_panel) {
    delete radar_panel;
  }
  if (m_draw_panel.draw) {
    delete m_draw_panel.draw;
    m_draw_panel.draw = 0;
  }
  if (m_draw_overlay.draw) {
    delete m_draw_overlay.draw;
    m_draw_overlay.draw = 0;
  }
  for (size_t z = 0; z < GUARD_ZONES; z++) {
    delete guard_zone[z];
  }
}

bool RadarInfo::Init(int verbose) {
  m_verbose = verbose;

  if (!transmit->Init(verbose)) {
    wxLogMessage(wxT("BR24radar_pi %s: Unable to create transmit socket"), name.c_str());
    return false;
  }

  radar_panel = new RadarPanel(m_pi, this, GetOCPNCanvasWindow());
  if (!radar_panel) {
    wxLogMessage(wxT("BR24radar_pi %s: Unable to create RadarPanel"), name.c_str());
    return false;
  }
  if (!radar_panel->Create()) {
    wxLogMessage(wxT("BR24radar_pi %s: Unable to create RadarCanvas"), name.c_str());
    return false;
  }
  return true;
}

void RadarInfo::SetName(wxString name) {
  if (name != this->name) {
    wxLogMessage(wxT("BR24radar_pi: Changing name of radar #%d from '%s' to '%s'"), radar, this->name.c_str(), name.c_str());
    this->name = name;
    radar_panel->SetCaption(name);
  }
}

void RadarInfo::StartReceive() {
  if (!receive) {
    wxLogMessage(wxT("BR24radar_pi: %s starting receive thread"), name.c_str());
    receive = new br24Receive(m_pi, &m_quit, this);
    receive->Run();
  }
}

void RadarInfo::ResetSpokes() {
  UINT8 zap[RETURNS_PER_LINE];

  memset(zap, 0, sizeof(zap));

  if (m_draw_panel.draw) {
    for (size_t r = 0; r < LINES_PER_ROTATION; r++) {
      m_draw_panel.draw->ProcessRadarSpoke(r, zap, sizeof(zap));
    }
  }
  if (m_draw_overlay.draw) {
    for (size_t r = 0; r < LINES_PER_ROTATION; r++) {
      m_draw_overlay.draw->ProcessRadarSpoke(r, zap, sizeof(zap));
    }
  }
  for (size_t z = 0; z < GUARD_ZONES; z++) {
    // Zap them anyway just to be sure
    guard_zone[z]->ResetBogeys();
  }
}

/*
 * A spoke of data has been received by the receive thread and it calls this (in
 * the context of
 * the receive thread, so no UI actions can be performed here.)
 *
 * @param angle                 Bearing (relative to Boat)  at which the spoke
 * is seen.
 * @param bearing               Bearing (relative to North) at which the spoke
 * is seen.
 * @param data                  A line of len bytes, each byte represents
 * strength at that distance.
 * @param len                   Number of returns
 * @param range                 Range (in meters) of this data
 * @param nowMillis             Timestamp when this was received
 */
void RadarInfo::ProcessRadarSpoke(SpokeBearing angle, SpokeBearing bearing, UINT8 *data, size_t len, int range_meters,
                                  wxLongLong nowMillis) {
  UINT8 *hist_data = history[angle];
  bool calc_history = multi_sweep_filter;
  wxMutexLocker lock(m_mutex);

  if (this->range_meters != range_meters) {
    // Wipe ALL spokes
    ResetSpokes();
    this->range_meters = range_meters;
    this->range.Update(range_meters);
    if (m_pi->m_settings.verbose) {
      wxLogMessage(wxT("BR24radar_pi: %s detected range %d"), name.c_str(), range_meters);
    }
  }
  // spoke[angle].age = nowMillis;

  if (!calc_history) {
    for (size_t z = 0; z < GUARD_ZONES; z++) {
      if (guard_zone[z]->type != GZ_OFF && guard_zone[z]->multi_sweep_filter) {
        calc_history = true;
      }
    }
  }

  if (calc_history) {
    for (size_t radius = 0; radius < len; radius++) {
      hist_data[radius] = hist_data[radius] << 1;  // shift left history byte 1 bit
      if (m_pi->m_color_map[data[radius]] != BLOB_NONE) {
        hist_data[radius] = hist_data[radius] | 1;  // and add 1 if above threshold
      }
    }
  }

  for (size_t z = 0; z < GUARD_ZONES; z++) {
    if (guard_zone[z]->type != GZ_OFF) {
      guard_zone[z]->ProcessSpoke(angle, data, hist_data, len, range_meters);
    }
  }

  if (multi_sweep_filter) {
    for (size_t radius = 0; radius < len; radius++) {
      if (data[radius] && !HISTORY_FILTER_ALLOW(hist_data[radius])) {
        data[radius] = 0;  // Zero out data here, so draw doesn't need to know
                           // about filtering
      }
    }
  }

  if (m_draw_panel.draw) {
    m_draw_panel.draw->ProcessRadarSpoke(rotation.value ? bearing : angle, data, len);
  }
  if (m_draw_overlay.draw) {
    m_draw_overlay.draw->ProcessRadarSpoke(bearing, data, len);
  }
}

void RadarInfo::RefreshDisplay(wxTimerEvent &event) {
  time_t now = time(0);
  int pos_age = difftime(now, m_pi->m_bpos_timestamp);  // the age of the
                                                        // position, last call of
                                                        // SetPositionFixEx
  if (m_refreshes_queued > 0 || pos_age >= 2) {
    // don't do additional refresh and reset the refresh conter
    // this will also balance performance, if too busy skip refresh
    // pos_age>=2 : OCPN too busy to pass position to pi, system overloaded
    // so skip next refresh
    if (m_verbose >= 5) {
      wxLogMessage(wxT("BR24radar_pi: %s busy encountered, pos_age = %d, refreshes_queued=%d"), name.c_str(), pos_age,
                   m_refreshes_queued);
    }
  } else {
    if (m_pi->m_settings.chart_overlay == this->radar) {
      m_refreshes_queued++;
      GetOCPNCanvasWindow()->Refresh(false);
    }
    m_refreshes_queued++;
    radar_panel->Refresh(false);
    if (m_verbose >= 4) {
      wxLogMessage(wxT("BR24radar_pi: %s refresh issued, queued = %d"), name.c_str(), m_refreshes_queued);
    }
  }

  // Calculate refresh speed
  if (m_pi->m_settings.refreshrate) {
    int millis = 1000 / (1 + ((m_pi->m_settings.refreshrate) - 1) * 5);

    if (millis != m_refresh_millis) {
      m_refresh_millis = millis;
      m_timer->Start(m_refresh_millis);
      wxLogMessage(wxT("BR24radar_pi: %s changed timer interval to %d milliseconds"), name.c_str(), m_refresh_millis);
    }
  }
}

void RadarInfo::RenderGuardZone(wxPoint radar_center, double v_scale_ppm) {
  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_HINT_BIT);  // Save state
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  int start_bearing = 0, end_bearing = 0;
  GLubyte red = 0, green = 200, blue = 0, alpha = 50;

  for (size_t z = 0; z < GUARD_ZONES; z++) {
    if (guard_zone[z]->type != GZ_OFF) {
      if (guard_zone[z]->type == GZ_CIRCLE) {
        start_bearing = 0;
        end_bearing = 359;
      } else {
        start_bearing = SCALE_RAW_TO_DEGREES2048(guard_zone[z]->start_bearing) - 90;
        end_bearing = SCALE_RAW_TO_DEGREES2048(guard_zone[z]->end_bearing) - 90;
      }
      switch (m_pi->m_settings.guard_zone_render_style) {
        case 1:
          glColor4ub((GLubyte)255, (GLubyte)0, (GLubyte)0, (GLubyte)255);
          DrawOutlineArc(guard_zone[z]->outer_range * v_scale_ppm, guard_zone[z]->inner_range * v_scale_ppm, start_bearing,
                         end_bearing, true);
          break;
        case 2:
          glColor4ub(red, green, blue, alpha);
          DrawOutlineArc(guard_zone[z]->outer_range * v_scale_ppm, guard_zone[z]->inner_range * v_scale_ppm, start_bearing,
                         end_bearing, false);
        // fall thru
        default:
          glColor4ub(red, green, blue, alpha);
          DrawFilledArc(guard_zone[z]->outer_range * v_scale_ppm, guard_zone[z]->inner_range * v_scale_ppm, start_bearing,
                        end_bearing);
      }
    }

    red = 0;
    green = 0;
    blue = 200;
  }

  glPopAttrib();
}

void RadarInfo::SetRangeMeters(int meters) {
  if (state.value == RADAR_TRANSMIT) {
    transmit->SetRange(meters);
  }
}

bool RadarInfo::SetControlValue(ControlType controlType, int value) { return transmit->SetControlValue(controlType, value); }

void RadarInfo::ShowRadarWindow(bool show) { radar_panel->ShowFrame(show); }

void RadarInfo::UpdateControlState(bool all) {
  wxMutexLocker lock(m_mutex);

  overlay.Update(m_pi->m_settings.chart_overlay == radar);
  if (overlay.value == 0 && m_draw_overlay.draw) {
    wxLogMessage(wxT("BR24radar_pi: Removing draw method as radar overlay is not shown"));
    delete m_draw_overlay.draw;
    m_draw_overlay.draw = 0;
  }
  if (!radar_panel->IsShown() && m_draw_panel.draw) {
    wxLogMessage(wxT("BR24radar_pi: Removing draw method as radar window is not shown"));
    delete m_draw_panel.draw;
    m_draw_panel.draw = 0;
  }

  if (control_dialog) {
    control_dialog->UpdateControlValues(all);
    control_dialog->UpdateDialogShown();
  }

  if (radar_panel->IsShown()) {
    radar_panel->Refresh(false);
  }
}

void RadarInfo::RenderRadarImage(wxPoint center, double scale, DrawInfo *di) {
  wxMutexLocker lock(m_mutex);
  int drawing_method = m_pi->m_settings.drawing_method;
  bool colorOption = m_pi->m_settings.display_option > 0;

  if (state.value != RADAR_TRANSMIT) {
    if (range_meters) {
      ResetSpokes();
      range_meters = 0;
    }
    return;
  }

  // Determine if a new draw method is required
  if (!di->draw || (drawing_method != di->drawing_method) || (colorOption != di->color_option)) {
    RadarDraw *newDraw = RadarDraw::make_Draw(m_pi, drawing_method);
    if (!newDraw) {
      wxLogMessage(wxT("BR24radar_pi: out of memory"));
      return;
    } else if (newDraw->Init(colorOption)) {
      wxArrayString methods;
      RadarDraw::GetDrawingMethods(methods);
      if (di == &m_draw_overlay) {
        wxLogMessage(wxT("BR24radar_pi: %s new drawing method %s for overlay"), name.c_str(), methods[drawing_method].c_str());
      } else {
        wxLogMessage(wxT("BR24radar_pi: %s new drawing method %s for panel"), name.c_str(), methods[drawing_method].c_str());
      }
      if (di->draw) {
        delete di->draw;
      }
      di->draw = newDraw;
      di->drawing_method = drawing_method;
      di->color_option = colorOption;
    } else {
      m_pi->m_settings.drawing_method = 0;
      delete newDraw;
    }
    if (!di->draw) {
      return;
    }
  }

  di->draw->DrawRadarImage(center, scale);
}

void RadarInfo::RenderRadarImage(wxPoint center, double scale, double rotation, bool overlay) {
  viewpoint_rotation = rotation;  // Will be picked up by next spoke calls

  if (overlay) {
    RenderRadarImage(center, scale, &m_draw_overlay);
  } else {
    RenderGuardZone(center, scale);
    RenderRadarImage(center, scale, &m_draw_panel);
  }

  if (m_refreshes_queued > 0) {
    m_refreshes_queued--;
  }
}

void RadarInfo::FlipRadarState() {
  if (state.button == RADAR_STANDBY) {
    transmit->RadarTxOn();
    state.button = RADAR_TRANSMIT;
    state.mod = true;
  } else {
    transmit->RadarTxOff();
    m_data_timeout = 0;
    state.button = RADAR_STANDBY;
    state.mod = true;
  }
}

wxString RadarInfo::GetCanvasTextTopLeft() {
  wxString s;

  if (rotation.value > 0) {
    s << _("North Up");
  } else {
    s << _("Head Up");
  }
  if (m_pi->m_settings.emulator_on) {
    s << wxT(" (");
    s << _("Emulator");
    s << wxT(")");
  } else if (control_dialog) {
    s << wxT("\n") << control_dialog->GetRangeText();
  }

  return s;
}

wxString RadarInfo::GetCanvasTextBottomLeft() { return m_pi->GetGuardZoneText(this, false); }

wxString RadarInfo::GetCanvasTextCenter() {
  wxString s;

  if (state.value == RADAR_OFF) {
    s << _("No radar");
  } else if (state.value == RADAR_STANDBY) {
    s << _("Standby");
    if (this->radar_type == RT_4G) {
      s << wxT(" 4G");
    }
  } else if (!m_draw_panel.draw) {
    s << _("No valid drawing method");
  }

  return s;
}

PLUGIN_END_NAMESPACE