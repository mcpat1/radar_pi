/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Radar Plugin
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
#include "ControlsDialog.h"
#include "GuardZone.h"
#include "MessageBox.h"
#include "RadarCanvas.h"
#include "RadarDraw.h"
#include "RadarFactory.h"
#include "RadarMarpa.h"
#include "RadarPanel.h"
#include "RadarReceive.h"
#include "TrailBuffer.h"
#include "drawutil.h"

PLUGIN_BEGIN_NAMESPACE

bool g_first_render = true;

/**
 * Constructor.
 *
 * Called when the config is not yet known, so this should not start any
 * computations based on those yet.
 */
RadarInfo::RadarInfo(radar_pi *pi, int radar) {
  m_pi = pi;
  m_radar = radar;
  m_arpa = 0;
  m_auto_range_mode = true;
  m_course_index = 0;
  m_old_range = 0;
  m_dir_lat = 0;
  m_dir_lon = 0;
  m_pixels_per_meter = 0.;
  m_auto_range_meters = 0;
  m_previous_auto_range_meters = 0;
  m_previous_orientation = ORIENTATION_HEAD_UP;
  m_stayalive_timeout = 0;
  m_radar_timeout = 0;
  m_data_timeout = 0;
  m_history = 0;
  m_polar_lookup = 0;
  m_spokes = 0;
  m_spoke_len_max = 0;
  m_trails = 0;
  m_idle_standby = 0;
  m_idle_transmit = 0;
  m_showManualValueInAuto = false;
  CLEAR_STRUCT(m_statistics);
  CLEAR_STRUCT(m_course_log);

  m_mouse_pos.lat = NAN;
  m_mouse_pos.lon = NAN;
  for (int i = 0; i < ORIENTATION_NUMBER; i++) {
    m_mouse_ebl[i] = NAN;
    m_mouse_vrm = NAN;
    for (int b = 0; b < BEARING_LINES; b++) {
      m_ebl[i][b] = NAN;
      m_vrm[b] = NAN;
    }
  }
  m_control = 0;
  m_receive = 0;
  m_draw_panel.draw = 0;
  m_draw_overlay.draw = 0;
  m_draw_time_ms = 1000;  // Assume really bad draw time until we actually measure it to prevent fast redraw at start
  m_radar_panel = 0;
  m_radar_canvas = 0;
  m_control_dialog = 0;
  m_state.Update(RADAR_OFF);
  m_refresh_millis = 50;

  for (size_t z = 0; z < GUARD_ZONES; z++) {
    m_guard_zone[z] = new GuardZone(m_pi, this, z);
  }
}

void RadarInfo::Shutdown() {
  if (m_receive) {
    wxLongLong threadStartWait = wxGetUTCTimeMillis();
    m_receive->Shutdown();
    m_receive->Wait();
    wxLongLong threadEndWait = wxGetUTCTimeMillis();

#ifdef NEVER
    wxLongLong threadExtraWait = 0;

    // See if Douwe is right and Wait() doesn't work properly -- attests it returns
    // before the thread is dead.
    while (!m_receive->m_is_shutdown) {
      wxYield();
      wxMilliSleep(10);
      threadExtraWait = wxGetUTCTimeMillis();
    }

    // Now log what we have done
    if (threadExtraWait != 0) {
      LOG_INFO(wxT("radar_pi: %s receive thread wait did not work, had to wait for %lu ms extra"), m_name.c_str(),
               threadExtraWait - threadEndWait);
      threadEndWait = threadExtraWait;
    }
    if (m_receive->m_shutdown_time_requested != 0) {
      LOG_INFO(wxT("radar_pi: %s receive thread stopped in %lu ms, had to wait for %lu ms"), m_name.c_str(),
               threadEndWait - m_receive->m_shutdown_time_requested, threadEndWait - threadStartWait);
    } else {
      LOG_INFO(wxT("radar_pi: %s receive thread stopped in %lu ms, had to wait for %lu ms"), m_name.c_str(),
               threadEndWait - m_receive->m_shutdown_time_requested, threadEndWait - threadStartWait);
    }
#endif

    LOG_INFO(wxT("radar_pi: %s receive thread stopped in %llu ms"), m_name.c_str(), threadEndWait - threadStartWait);

    delete m_receive;
    m_receive = 0;
  }

  if (m_control_dialog) {
    delete m_control_dialog;
    m_control_dialog = 0;
  }
  if (m_radar_panel) {
    delete m_radar_panel;
    m_radar_panel = 0;
  }
}

RadarInfo::~RadarInfo() {
  Shutdown();

  if (m_draw_panel.draw) {
    delete m_draw_panel.draw;
    m_draw_panel.draw = 0;
  }
  if (m_draw_overlay.draw) {
    delete m_draw_overlay.draw;
    m_draw_overlay.draw = 0;
  }
  if (m_control) {
    delete m_control;
    m_control = 0;
  }
  if (m_arpa) {
    delete m_arpa;
    m_arpa = 0;
  }
  if (m_trails) {
    delete m_trails;
    m_trails = 0;
  }
  for (size_t z = 0; z < GUARD_ZONES; z++) {
    if (m_guard_zone[z]) {
      delete m_guard_zone[z];
      m_guard_zone[z] = 0;
    }
  }

  if (m_history) {
    for (size_t i = 0; i < m_spokes; i++) {
      if (m_history[i].line) {
        free(m_history[i].line);
      }
    }
    free(m_history);
  }
}

/**
 * Initialize the on-screen and receive/transmit items.
 *
 * This is called after the config file has been loaded, so all state is known.
 * It is also called when the user reselects radars, so it needs to be able to be called
 * multiple times.
 */
bool RadarInfo::Init() {
  m_verbose = M_SETTINGS.verbose;
  m_name = RadarTypeName[m_radar_type];
  m_spokes = RadarSpokes[m_radar_type];
  m_spoke_len_max = RadarSpokeLenMax[m_radar_type];

  m_history = (line_history *)calloc(sizeof(line_history), m_spokes);
  for (size_t i = 0; i < m_spokes; i++) {
    m_history[i].line = (uint8_t *)calloc(sizeof(uint8_t), m_spoke_len_max);
  }
  m_polar_lookup = new PolarToCartesianLookup(m_spokes, m_spoke_len_max);

  ComputeColourMap();

  if (!m_control) {
    m_control = RadarFactory::MakeRadarControl(m_radar_type);
  }
  if (!m_radar_panel) {
    m_radar_panel = new RadarPanel(m_pi, this, GetOCPNCanvasWindow());
    if (!m_radar_panel || !m_radar_panel->Create()) {
      wxLogError(wxT("radar_pi %s: Unable to create RadarPanel"), m_name.c_str());
      return false;
    }
  }
  if (!m_arpa) {
    m_arpa = new RadarArpa(m_pi, this);
  }
  m_trails = new TrailBuffer(this, m_spokes, m_spoke_len_max);
  ComputeTargetTrails();

  UpdateControlState(true);

  if (!m_receive) {
    LOG_RECEIVE(wxT("radar_pi: %s starting receive thread"), m_name.c_str());
    m_receive = RadarFactory::MakeRadarReceive(m_radar_type, m_pi, this);
    if (!m_receive || (m_receive->Run() != wxTHREAD_NO_ERROR)) {
      LOG_INFO(wxT("radar_pi: %s unable to start receive thread."), m_name.c_str());
      if (m_receive) {
        delete m_receive;
      }
      m_receive = 0;
    }
  }

  return true;
}

void RadarInfo::ShowControlDialog(bool show, bool reparent) {
  if (show) {
    wxPoint panel_pos = wxDefaultPosition;
    bool manually_positioned = false;

    if (m_control_dialog && reparent) {
      panel_pos = m_control_dialog->m_panel_position;
      manually_positioned = m_control_dialog->m_manually_positioned;
      delete m_control_dialog;
      m_control_dialog = 0;
      LOG_VERBOSE(wxT("radar_pi %s: Reparenting control dialog"), m_name.c_str());
    }
    if (!m_control_dialog) {
      m_control_dialog = RadarFactory::MakeControlsDialog(m_radar_type, m_radar);
      m_control_dialog->m_panel_position = panel_pos;
      m_control_dialog->m_manually_positioned = manually_positioned;
      wxWindow *parent = (wxWindow *)m_radar_panel;
      if (!m_pi->m_settings.show_radar[m_radar]) {
        parent = GetOCPNCanvasWindow();
      }
      LOG_VERBOSE(wxT("radar_pi %s: Creating control dialog"), m_name.c_str());
      m_control_dialog->Create(parent, m_pi, this, wxID_ANY, m_name, m_pi->m_settings.control_pos[m_radar]);
    }
    m_control_dialog->ShowDialog();
  } else if (m_control_dialog) {
    m_control_dialog->HideDialog();
  }
}

void RadarInfo::DetectedRadar(NetworkAddress &interfaceAddress, NetworkAddress &radarAddress) {
  m_pi->SetRadarInterfaceAddress(m_radar, interfaceAddress);
  if (!m_control->Init(m_pi, this, interfaceAddress, radarAddress)) {
    wxLogError(wxT("radar_pi %s: Unable to create transmit socket"), m_name.c_str());
  }
  m_stayalive_timeout = 0;  // Allow immediate restart of any TxOn or TxOff command
  m_pi->NotifyControlDialog();
}

void RadarInfo::SetName(wxString name) {
  if (name != m_name) {
    LOG_DIALOG(wxT("radar_pi: Changing name of radar #%d from '%s' to '%s'"), m_radar, m_name.c_str(), name.c_str());
    m_name = name;
    m_radar_panel->SetCaption(name);
    if (m_control_dialog) {
      m_control_dialog->SetTitle(name);
    }
  }
}

void RadarInfo::ComputeColourMap() {
  for (int i = 0; i <= UINT8_MAX; i++) {
    m_colour_map[i] = (i >= m_pi->m_settings.threshold_red) ? BLOB_STRONG
                                                            : (i >= m_pi->m_settings.threshold_green)
                                                                  ? BLOB_INTERMEDIATE
                                                                  : (i >= m_pi->m_settings.threshold_blue) ? BLOB_WEAK : BLOB_NONE;
  }

  for (int i = 0; i < BLOB_COLOURS; i++) {
    m_colour_map_rgb[i] = wxColour(0, 0, 0);
  }
  m_colour_map_rgb[BLOB_STRONG] = m_pi->m_settings.strong_colour;
  m_colour_map_rgb[BLOB_INTERMEDIATE] = m_pi->m_settings.intermediate_colour;
  m_colour_map_rgb[BLOB_WEAK] = m_pi->m_settings.weak_colour;

  if (m_trails_motion.GetValue() > 0) {
    float r1 = m_pi->m_settings.trail_start_colour.Red();
    float g1 = m_pi->m_settings.trail_start_colour.Green();
    float b1 = m_pi->m_settings.trail_start_colour.Blue();
    float r2 = m_pi->m_settings.trail_end_colour.Red();
    float g2 = m_pi->m_settings.trail_end_colour.Green();
    float b2 = m_pi->m_settings.trail_end_colour.Blue();
    float delta_r = (r2 - r1) / BLOB_HISTORY_COLOURS;
    float delta_g = (g2 - g1) / BLOB_HISTORY_COLOURS;
    float delta_b = (b2 - b1) / BLOB_HISTORY_COLOURS;

    for (BlobColour history = BLOB_HISTORY_0; history <= BLOB_HISTORY_MAX; history = (BlobColour)(history + 1)) {
      m_colour_map[history] = history;

      m_colour_map_rgb[history] = wxColour(r1, g1, b1);
      r1 += delta_r;
      g1 += delta_g;
      b1 += delta_b;
    }
  }
}

void RadarInfo::ResetSpokes() {
  uint8_t zap[SPOKE_LEN_MAX];

  LOG_VERBOSE(wxT("radar_pi: reset spokes"));

  CLEAR_STRUCT(zap);
  for (size_t i = 0; i < m_spokes; i++) {
    memset(m_history[i].line, 0, m_spoke_len_max);
    m_history[i].time = 0;
    m_history[i].pos.lat = 0.;
    m_history[i].pos.lon = 0.;
  }

  if (m_draw_panel.draw) {
    for (size_t r = 0; r < m_spokes; r++) {
      m_draw_panel.draw->ProcessRadarSpoke(0, r, zap, m_spoke_len_max);
    }
  }
  if (m_draw_overlay.draw) {
    for (size_t r = 0; r < m_spokes; r++) {
      m_draw_overlay.draw->ProcessRadarSpoke(0, r, zap, m_spoke_len_max);
    }
  }

  for (size_t z = 0; z < GUARD_ZONES; z++) {
    // Zap them anyway just to be sure
    m_guard_zone[z]->ResetBogeys();
  }
}

/*
 * A spoke of data has been received by the receive thread and it calls this (in
 * the context of the receive thread, so no UI actions can be performed here.)
 *
 * @param angle                 Bearing (relative to Boat)  at which the spoke is seen.
 * @param bearing               Bearing (relative to North) at which the spoke is seen.
 * @param data                  A line of len bytes, each byte represents strength at that distance.
 * @param len                   Number of returns
 * @param range                 Range (in meters) of this data
 * @param time_rec              Time at this moment
 */
void RadarInfo::ProcessRadarSpoke(SpokeBearing angle, SpokeBearing bearing, uint8_t *data, size_t len, int range_meters,
                                  wxLongLong time_rec) {
  int orientation;

  // calculate course as the moving average of m_hdt over one revolution
  SampleCourse(angle);  // used for course_up mode

  for (int i = 0; i < m_main_bang_size.GetValue(); i++) {
    data[i] = 0;
  }

  // Recompute 'pixels_per_meter' based on the actual spoke length and range in meters.
  double pixels_per_meter = len / (double)range_meters;

  if (m_pixels_per_meter != pixels_per_meter) {
    LOG_VERBOSE(wxT("radar_pi: %s detected spoke range change from %g to %g pixels/m, %d meters"), m_name.c_str(),
                m_pixels_per_meter, pixels_per_meter, range_meters);
    m_pixels_per_meter = pixels_per_meter;
    ResetSpokes();
    if (m_arpa) {
      m_arpa->ClearContours();
    }
  }
  
  orientation = GetOrientation();
  if ((orientation == ORIENTATION_HEAD_UP || m_previous_orientation == ORIENTATION_HEAD_UP) &&
      (orientation != m_previous_orientation)) {
    ResetSpokes();
    m_previous_orientation = orientation;
  }

  // In NORTH or COURSE UP modes we store the radar data at the bearing received
  // in the spoke. In other words: at an absolute angle off north.
  // This way, when the boat rotates the data on the overlay doesn't rotate with it.
  // This is also called 'stabilized' mode, I guess.
  //
  // The history data used for the ARPA data is *always* in bearing mode, it is not usable
  // with relative data.
  //
  int stabilized_mode = orientation != ORIENTATION_HEAD_UP;
  uint8_t weakest_normal_blob = m_pi->m_settings.threshold_blue;

  uint8_t *hist_data = m_history[bearing].line;
  m_history[bearing].time = time_rec;
  memset(hist_data, 0, m_spoke_len_max);
  GetRadarPosition(&m_history[bearing].pos);
  for (size_t radius = 0; radius < len; radius++) {
    if (data[radius] >= weakest_normal_blob) {
      // and add 1 if above threshold and set the left 2 bits, used for ARPA
      hist_data[radius] = 192;
    }
  }

  for (size_t z = 0; z < GUARD_ZONES; z++) {
    if (m_guard_zone[z]->m_alarm_on) {
      m_guard_zone[z]->ProcessSpoke(angle, data, m_history[bearing].line, len);
    }
  }

  bool draw_trails_on_overlay = (m_pi->m_settings.trails_on_overlay == 1);
  if (m_draw_overlay.draw && !draw_trails_on_overlay) {
    m_draw_overlay.draw->ProcessRadarSpoke(M_SETTINGS.overlay_transparency.GetValue(), bearing, data, len);
  }

  m_trails->UpdateTrailPosition();

  // True trails
  m_trails->UpdateTrueTrails(bearing, data, len);

  // Relative trails
  m_trails->UpdateRelativeTrails(angle, data, len);

  if (m_pi->m_settings.show_extreme_range) {
    data[len - 1] = 255;
    data[1] = 255;  // Main bang on purpose to show radar center
    data[0] = 255;  // Main bang on purpose to show radar center
  }

  if (m_draw_overlay.draw && draw_trails_on_overlay) {
    m_draw_overlay.draw->ProcessRadarSpoke(M_SETTINGS.overlay_transparency.GetValue(), bearing, data, len);
  }

  if (m_draw_panel.draw) {
    m_draw_panel.draw->ProcessRadarSpoke(4, stabilized_mode ? bearing : angle, data, len);
  }
}

void RadarInfo::SampleCourse(int angle) {
  //  Calculates the moving average of m_hdt and returns this in m_course
  //  This is a bit more complicated then expected, average of 359 and 1 is 180 and that is not what we want
  if (m_pi->GetHeadingSource() != HEADING_NONE && ((angle & 127) == 0)) {  // sample m_hdt every 128 spokes
    if (m_course_log[m_course_index] > 720.) {                             // keep values within limits
      for (int i = 0; i < COURSE_SAMPLES; i++) {
        m_course_log[i] -= 720;
      }
    }
    if (m_course_log[m_course_index] < -720.) {
      for (int i = 0; i < COURSE_SAMPLES; i++) {
        m_course_log[i] += 720;
      }
    }
    double hdt = m_pi->GetHeadingTrue();
    while (m_course_log[m_course_index] - hdt > 180.) {  // compare with previous value
      hdt += 360.;
    }
    while (m_course_log[m_course_index] - hdt < -180.) {
      hdt -= 360.;
    }
    m_course_index++;
    if (m_course_index >= COURSE_SAMPLES) m_course_index = 0;
    m_course_log[m_course_index] = hdt;
    double sum = 0;
    for (int i = 0; i < COURSE_SAMPLES; i++) {
      sum += m_course_log[i];
    }
    m_course = fmod(sum / COURSE_SAMPLES + 720., 360);
  }
}

void RadarInfo::UpdateTransmitState() {
  wxCriticalSectionLocker lock(m_exclusive);
  time_t now = time(0);

  int state = m_state.GetValue();

  if (state == RADAR_TRANSMIT && TIMED_OUT(now, m_data_timeout)) {
    m_state.Update(RADAR_STANDBY);
    LOG_INFO(wxT("radar_pi: %s data lost"), m_name.c_str());
  }
  if (state == RADAR_STANDBY && TIMED_OUT(now, m_radar_timeout)) {
    static wxString empty;

    m_state.Update(RADAR_OFF);
    LOG_INFO(wxT("radar_pi: %s lost presence"), m_name.c_str());
    return;
  }

  if (!m_pi->IsRadarOnScreen(m_radar)) {
    return;
  }

  if (state == RADAR_TRANSMIT && TIMED_OUT(now, m_stayalive_timeout)) {
    m_control->RadarStayAlive();
    m_stayalive_timeout = now + STAYALIVE_TIMEOUT;
  }

  // If we find we have a radar and the boot flag is still set, turn radar on
  // Think about interaction with timed_transmit
  if (m_boot_state.GetValue() == RADAR_TRANSMIT && state == RADAR_STANDBY) {
    m_boot_state.Update(RADAR_OFF);
    RequestRadarState(RADAR_TRANSMIT);
  }
}

void RadarInfo::RequestRadarState(RadarState state) {
  int oldState = m_state.GetValue();

  if (m_pi->IsRadarOnScreen(m_radar) && oldState != RADAR_OFF) {                         // if radar is visible and detected
    if (oldState != state && !(oldState != RADAR_STANDBY && state == RADAR_TRANSMIT)) {  // and change is wanted
      time_t now = time(0);

      switch (state) {
        case RADAR_TRANSMIT:
          m_control->RadarTxOn();
          // Refresh radar immediately so that we generate draw mechanisms
          if (m_pi->m_settings.chart_overlay == m_radar) {
            GetOCPNCanvasWindow()->Refresh(false);
          }
          if (m_radar_panel) {
            m_radar_panel->Refresh();
          }
          break;

        case RADAR_STANDBY:
          m_control->RadarTxOff();
          break;

        case RADAR_SPINNING_UP:
        case RADAR_TIMED_IDLE:
        case RADAR_WARMING_UP:
        case RADAR_OFF:
          LOG_INFO(wxT("radar_pi: %s unexpected status request %d"), m_name.c_str(), state);
      }
      m_stayalive_timeout = now + STAYALIVE_TIMEOUT;
    }
  }
}

void RadarInfo::RenderGuardZone() {
  int start_bearing = 0, end_bearing = 0;
  GLubyte red = 0, green = 200, blue = 0, alpha = 50;

  for (size_t z = 0; z < GUARD_ZONES; z++) {
    if (m_guard_zone[z]->m_alarm_on || m_guard_zone[z]->m_arpa_on || m_guard_zone[z]->m_show_time + 5 > time(0)) {
      if (m_guard_zone[z]->m_type == GZ_CIRCLE) {
        start_bearing = 0;
        end_bearing = 359;
      } else {
        start_bearing = m_guard_zone[z]->m_start_bearing;
        end_bearing = m_guard_zone[z]->m_end_bearing;
      }
      switch (m_pi->m_settings.guard_zone_render_style) {
        case 1:
          glColor4ub((GLubyte)255, (GLubyte)0, (GLubyte)0, (GLubyte)255);
          DrawOutlineArc(m_guard_zone[z]->m_outer_range, m_guard_zone[z]->m_inner_range, start_bearing, end_bearing, true);
          break;
        case 2:
          glColor4ub(red, green, blue, alpha);
          DrawOutlineArc(m_guard_zone[z]->m_outer_range, m_guard_zone[z]->m_inner_range, start_bearing, end_bearing, false);
        // fall thru
        default:
          glColor4ub(red, green, blue, alpha);
          DrawFilledArc(m_guard_zone[z]->m_outer_range, m_guard_zone[z]->m_inner_range, start_bearing, end_bearing);
      }
    }

    red = 0;
    green = 0;
    blue = 200;
  }

  start_bearing = m_no_transmit_start.GetValue();
  end_bearing = m_no_transmit_end.GetValue();
  int range = m_range.GetValue();
  if (start_bearing != end_bearing && start_bearing >= -180 && end_bearing >= -180 && range != 0) {
    if (start_bearing < 0) {
      start_bearing += 360;
    }
    if (end_bearing < 0) {
      end_bearing += 360;
    }
    glColor4ub(250, 255, 255, alpha);
    DrawFilledArc(range, 0, m_no_transmit_start.GetValue(), m_no_transmit_end.GetValue());
  }
}

void RadarInfo::SetAutoRangeMeters(int meters) {
  if (m_state.GetValue() == RADAR_TRANSMIT && m_auto_range_mode) {
    m_auto_range_meters = meters;
    // Don't adjust auto range meters continuously when it is oscillating a little bit (< 5%)
    int test = 100 * m_previous_auto_range_meters / m_auto_range_meters;
    if (test < 95 || test > 105) {  //   range change required
      // Compute a 'standard' distance. This will be slightly smaller.
      meters = GetNearestRange(meters, m_pi->m_settings.range_units);
      if (meters != m_range.GetValue()) {
        if (m_pi->m_settings.verbose) {
          LOG_VERBOSE(wxT("radar_pi: Automatic range changed from %d to %d meters"), m_previous_auto_range_meters,
                      m_auto_range_meters);
        }
        m_control->SetRange(meters);
        m_previous_auto_range_meters = m_auto_range_meters;
      }
    }
  } else {
    m_previous_auto_range_meters = 0;
  }
}

bool RadarInfo::SetControlValue(ControlType controlType, RadarControlItem &item) {
  return m_control->SetControlValue(controlType, item);
}

void RadarInfo::ShowRadarWindow(bool show) { m_radar_panel->ShowFrame(show); }

bool RadarInfo::IsPaneShown() { return m_radar_panel->IsPaneShown(); }

void RadarInfo::UpdateControlState(bool all) {
  wxCriticalSectionLocker lock(m_exclusive);

  m_overlay.Update(m_pi->m_settings.chart_overlay == m_radar);

#ifdef OPENCPN_NO_LONGER_MIXES_GL_CONTEXT
  //
  // Once OpenCPN doesn't mess up with OpenGL context anymore we can do this
  //
  if (m_overlay.value == 0 && m_draw_overlay.draw) {
    LOG_DIALOG(wxT("radar_pi: Removing draw method as radar overlay is not shown"));
    delete m_draw_overlay.draw;
    m_draw_overlay.draw = 0;
  }
  if (!IsShown() && m_draw_panel.draw) {
    LOG_DIALOG(wxT("radar_pi: Removing draw method as radar window is not shown"));
    delete m_draw_panel.draw;
    m_draw_panel.draw = 0;
  }
#endif

  if (m_control_dialog) {
    m_control_dialog->UpdateControlValues(all);
  }

  if (IsPaneShown()) {
    m_radar_panel->Refresh(false);
  }
}

void RadarInfo::ResetRadarImage() {
  if (m_pixels_per_meter != 0.) {
    ResetSpokes();
    ClearTrails();
    if (m_arpa) {
      m_arpa->ClearContours();
    }
    m_pixels_per_meter = 0.;
  }
}

/**
 * plugin calls this to request a redraw of the PPI window.
 *
 * Called on GUI thread.
 */
void RadarInfo::RefreshDisplay() {
  if (IsPaneShown()) {
    m_radar_panel->Refresh(false);
  }
}

void RadarInfo::RenderRadarImage(DrawInfo *di) {
  wxCriticalSectionLocker lock(m_exclusive);
  int drawing_method = m_pi->m_settings.drawing_method;
  int state = m_state.GetValue();

  if (state != RADAR_TRANSMIT) {
    ResetRadarImage();
    return;
  }

  // Determine if a new draw method is required
  if (!di->draw || (drawing_method != di->drawing_method)) {
    RadarDraw *newDraw = RadarDraw::make_Draw(this, drawing_method);
    if (!newDraw) {
      wxLogError(wxT("radar_pi: out of memory"));
      return;
    } else if (newDraw->Init(m_spokes, m_spoke_len_max)) {
      wxArrayString methods;
      RadarDraw::GetDrawingMethods(methods);
      if (di == &m_draw_overlay) {
        LOG_VERBOSE(wxT("radar_pi: %s new drawing method %s for overlay"), m_name.c_str(), methods[drawing_method].c_str());
      } else {
        LOG_VERBOSE(wxT("radar_pi: %s new drawing method %s for panel"), m_name.c_str(), methods[drawing_method].c_str());
      }
      if (di->draw) {
        delete di->draw;
      }
      di->draw = newDraw;
      di->drawing_method = drawing_method;
    } else {
      m_pi->m_settings.drawing_method = 0;
      delete newDraw;
    }
    if (!di->draw) {
      return;
    }
  }

  di->draw->DrawRadarImage();
  if (g_first_render) {
    g_first_render = false;
    wxLongLong startup_elapsed = wxGetUTCTimeMillis() - m_pi->GetBootMillis();
    LOG_INFO(wxT("radar_pi: First radar image rendered after %llu ms\n"), startup_elapsed);
  }
}

int RadarInfo::GetOrientation() {
  int orientation;

  // check for no longer allowed value
  if (m_pi->GetHeadingSource() == HEADING_NONE) {
    orientation = ORIENTATION_HEAD_UP;
  } else {
    orientation = m_orientation.GetValue();
  }

  return orientation;
}

void RadarInfo::RenderRadarImage(wxPoint center, double scale, double overlay_rotate, bool overlay) {
  if (m_pixels_per_meter == 0.) {
    return;
  }
  bool arpa_on = false;
  if (m_arpa) {
    for (int i = 0; i < GUARD_ZONES; i++) {
      if (m_guard_zone[i]->m_arpa_on) arpa_on = true;
    }
    if (m_arpa->GetTargetCount()) {
      arpa_on = true;
    }
  }

  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_HINT_BIT);  // Save state
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  overlay_rotate += OPENGL_ROTATION;  // Difference between OpenGL and compass + radar
                                      // Note that for overlay=false this is purely OPENGL_ROTATION.

  double panel_rotate = overlay_rotate;
  double guard_rotate = overlay_rotate;
  double arpa_rotate;

  // So many combinations here

  int orientation = GetOrientation();
  int range = m_range.GetValue();

  if (!overlay) {
    arpa_rotate = 0.;
    switch (orientation) {
      case ORIENTATION_STABILIZED_UP:
        panel_rotate -= m_course;  // Panel only needs stabilized heading applied
        arpa_rotate -= m_course;
        guard_rotate += m_pi->GetHeadingTrue() - m_course;
        break;
      case ORIENTATION_COG_UP: {
        double cog = m_pi->GetCOG();
        panel_rotate -= cog;  // Panel only needs stabilized heading applied
        arpa_rotate -= cog;
        guard_rotate += m_pi->GetHeadingTrue() - cog;
      } break;
      case ORIENTATION_NORTH_UP:
        guard_rotate += m_pi->GetHeadingTrue();
        break;
      case ORIENTATION_HEAD_UP:
        arpa_rotate += -m_pi->GetHeadingTrue();  // Undo the actual heading calculation always done for ARPA
        break;
    }
  } else {
    guard_rotate += m_pi->GetHeadingTrue();
    arpa_rotate = overlay_rotate - OPENGL_ROTATION;
  }

  if (arpa_on) {
    m_arpa->RefreshArpaTargets();
  }

  if (overlay) {
    if (m_pi->m_settings.guard_zone_on_overlay) {
      glPushMatrix();
      glTranslated(center.x, center.y, 0);
      glRotated(guard_rotate, 0.0, 0.0, 1.0);
      glScaled(scale, scale, 1.);

      // LOG_DIALOG(wxT("radar_pi: %s render guard zone on overlay"), m_name.c_str());
      RenderGuardZone();
      glPopMatrix();
    }

    double radar_scale = scale / m_pixels_per_meter;
    glPushMatrix();
    glTranslated(center.x, center.y, 0);
    glRotated(panel_rotate, 0.0, 0.0, 1.0);
    glScaled(radar_scale, radar_scale, 1.);

    RenderRadarImage(&m_draw_overlay);
    glPopMatrix();

    if (arpa_on) {
      glPushMatrix();
      glTranslated(center.x, center.y, 0);
      LOG_VERBOSE(wxT("radar_pi: %s render ARPA targets on overlay with rot=%f"), m_name.c_str(), arpa_rotate);

      glRotated(arpa_rotate, 0.0, 0.0, 1.0);
      glScaled(scale, scale, 1.);
      m_arpa->DrawArpaTargets();
      glPopMatrix();
    }

  } else if (range != 0) {
    wxStopWatch stopwatch;

    glPushMatrix();
    scale = 1.0 / range;
    glRotated(guard_rotate, 0.0, 0.0, 1.0);
    glScaled(scale, scale, 1.);
    RenderGuardZone();
    glPopMatrix();

    glPushMatrix();
    double radar_scale = scale / m_pixels_per_meter;
    glScaled(radar_scale, radar_scale, 1.);
    glRotated(panel_rotate, 0.0, 0.0, 1.0);
    LOG_DIALOG(wxT("radar_pi: %s render scale=%g radar_scale=%g"), m_name.c_str(), scale, radar_scale);
    RenderRadarImage(&m_draw_panel);
    glPopMatrix();

    if (arpa_on) {
      glPushMatrix();
      glScaled(scale, scale, 1.);
      glRotated(arpa_rotate, 0.0, 0.0, 1.0);
      m_arpa->DrawArpaTargets();
      glPopMatrix();
    }
    glFinish();
    m_draw_time_ms = stopwatch.Time();
  }

  glPopAttrib();
}

wxString RadarInfo::GetCanvasTextTopLeft() {
  wxString s;

  switch (GetOrientation()) {
    case ORIENTATION_HEAD_UP:
      s << _("Head Up");
      break;
    case ORIENTATION_STABILIZED_UP:
      s << _("Head Up") << wxT("\n") << _("Stabilized");
      break;
    case ORIENTATION_COG_UP:
      s << _("Course Up");
      break;
    case ORIENTATION_NORTH_UP:
      s << _("North Up");
      break;
    default:
      s << _("Unknown");
      break;
  }
  if (m_range.GetValue() != 0) {
    s << wxT("\n") << GetRangeText();
  }
  if (s.Right(1) != wxT("\n")) {
    s << wxT("\n");
  }

  int motion = m_trails_motion.GetValue();
  if (motion != TARGET_MOTION_OFF) {
    if (motion == TARGET_MOTION_TRUE) {
      s << wxT("RM(T)");
    } else {
      s << wxT("RM(R)");
    }
  } else {
    s << wxT("RM");
  }

  return s;
}

wxString RadarInfo::FormatDistance(double distance) {
  wxString s;

  if (m_pi->m_settings.range_units > 0) {
    distance *= 1.852;

    if (distance < 1.000) {
      int meters = distance * 1000.0;
      s << meters;
      s << "m";
    } else {
      s << wxString::Format(wxT("%.2fkm"), distance);
    }
  } else {
    if (distance < 0.25 * 1.852) {
      int meters = distance * 1852.0;
      s << meters;
      s << "m";
    } else {
      s << wxString::Format(wxT("%.2fnm"), distance);
    }
  }

  return s;
}

wxString RadarInfo::FormatAngle(double angle) {
  wxString s;

  wxString relative;
  if (angle > 360) angle -= 360;
  if (GetOrientation() != ORIENTATION_HEAD_UP) {
    relative = wxT("T");
  } else {
    if (angle > 180.0) {
      angle = -(360.0 - angle);
    }
    relative = wxT("R");
  }
  s << wxString::Format(wxT("%.1f\u00B0%s"), angle, relative);

  return s;
}

wxString RadarInfo::GetCanvasTextBottomLeft() {
  GeoPosition radar_pos;
  wxString s = m_pi->GetGuardZoneText(this);

  if (m_state.GetValue() == RADAR_TRANSMIT) {
    double distance = 0.0, bearing = nan("");
    int orientation = GetOrientation();

    // Add VRM/EBLs

    for (int b = 0; b < BEARING_LINES; b++) {
      double bearing = m_ebl[orientation][b];
      if (!isnan(m_vrm[b]) && !isnan(bearing)) {
        if (orientation == ORIENTATION_STABILIZED_UP) {
          bearing += m_course;
          if (bearing >= 360) bearing -= 360;
        }

        if (s.length()) {
          s << wxT("\n");
        }
        s << wxString::Format(wxT("VRM%d=%s EBL%d=%s"), b + 1, FormatDistance(m_vrm[b]), b + 1, FormatAngle(bearing));
      }
    }
    // Add in mouse cursor location

    if (!isnan(m_mouse_vrm)) {
      distance = m_mouse_vrm;
      bearing = m_mouse_ebl[orientation];

      if (orientation == ORIENTATION_STABILIZED_UP) {
        bearing += m_course;
      } else if (orientation == ORIENTATION_COG_UP) {
        bearing += m_pi->GetCOG();
      }
      if (bearing >= 360) bearing -= 360;

    } else if (!isnan(m_mouse_pos.lat) && !isnan(m_mouse_pos.lon) && GetRadarPosition(&radar_pos)) {
      // Can't compute this upfront, ownship may move...
      distance = local_distance(radar_pos, m_mouse_pos);
      bearing = local_bearing(radar_pos, m_mouse_pos);
      if (GetOrientation() != ORIENTATION_NORTH_UP) {
        bearing -= m_pi->GetHeadingTrue();
      }
    }

    if (distance != 0.0) {
      if (s.length()) {
        s << wxT("\n");
      }
      s << FormatDistance(distance) << wxT(", ") << FormatAngle(bearing);
    }
  }
  return s;
}

wxString RadarInfo::GetCanvasTextCenter() {
  wxString s;

  switch (m_state.GetValue()) {
    case RADAR_OFF:
      s << _("No radar");
      break;
    case RADAR_STANDBY:
      s << _("Radar is in Standby");
      break;
    case RADAR_WARMING_UP:
      s << _("Radar warming up") << wxString::Format(wxT(" (%d s)"), m_warmup.GetValue());
      break;
    case RADAR_SPINNING_UP:
      s << _("Radar is spinning up");
      break;
    case RADAR_TRANSMIT:
      if (m_draw_panel.draw) {
        return s;
      }
      s << _("Radar not transmitting");
      break;
  }

  s << wxT("\n") << m_name;

  return s;
}

wxString RadarInfo::GetRangeText() {
  int meters = m_range.GetValue();

  bool auto_range = m_auto_range_mode && (m_overlay.GetValue() > 0);

  m_range_text = wxT("");
  if (auto_range) {
    m_range_text = _("Auto");
    m_range_text << wxT(" (");
  }

  wxString s = GetDisplayRangeStr(meters, true);
  if (s.length() == 0) {
    s = wxString::Format(wxT("/%d m/"), meters);
  }
  m_range_text << s;

  if (auto_range) {
    m_range_text << wxT(")");
  }

  LOG_DIALOG(wxT("radar_pi: range label '%s' for range=%d auto=%d"), m_range_text.c_str(), meters, m_auto_range_mode);
  return m_range_text;
}

/*
 * Create a nice value for 1/4, 1/2, 3/4 or 1/1 of the range.
 *
 * We only have a value in meters, and based on that we decide
 * whether it is likely a value in metric or nautical miles.
 *
 * Return empty string if it is not representable nicely
 *
 * @return String with human readable representation of range
 */
wxString RadarInfo::GetDisplayRangeStr(int meters, bool unit) {
  wxString s;

  if ((meters < 100 && meters % 25 == 0) || (meters < 1000 && meters % 50 == 0) || (meters % 1000 == 0)) {
    // really sure this is metric.

    if (meters % 25 == 0) {
      s = wxString::Format(wxT("%d"), meters);
      if (unit) {
        s << " m";
      }
    }
    return s;
  }

  if (meters % NM(1) == 0) {
    s = wxString::Format(wxT("%d"), meters / NM(1));
  } else if (meters > NM(1) && meters % NM(1) == NM(1) / 2) {
    s = wxString::Format(wxT("%d.5"), meters / NM(1));
  } else {
    switch (meters) {
      case NM(1 / 4):
        s = wxT("1/4");
        break;
      case NM(1 / 2):
        s = wxT("1/2");
        break;
      case NM(3 / 4):
        s = wxT("3/4");
        break;
      case NM(1 / 8):
      case NM(1 / 8) + 1:
        s = wxT("1/8");
        break;
      case NM(3 / 8):
      case NM(3 / 8) + 1:
        s = wxT("3/4");
        break;
      case NM(1 / 16):
      case NM(1 / 16) + 1:
        s = wxT("1/16");
        break;
      case NM(3 / 16):
      case NM(3 / 16) + 1:
        s = wxT("3/16");
        break;
      case NM(1 / 32):
      case NM(1 / 32) + 1:
        s = wxT("1/32");
        break;
      case NM(3 / 32):
      case NM(3 / 32) + 1:
        s = wxT("3/32");
        break;
      default:
        return wxT("");
    }
  }
  if (unit) {
    s << wxT(" NM");
  }
  return s;
}

void RadarInfo::SetMousePosition(GeoPosition pos) {
  for (int i = 0; i < ORIENTATION_NUMBER; i++) {
    m_mouse_ebl[i] = NAN;
  }
  m_mouse_vrm = NAN;
  m_mouse_pos = pos;
  LOG_DIALOG(wxT("radar_pi: SetMousePosition(%f, %f)"), pos.lat, pos.lon);
}

void RadarInfo::SetMouseVrmEbl(double vrm, double ebl) {
  double bearing;
  int orientation = GetOrientation();
  double cog = m_pi->GetCOG();

  m_mouse_vrm = vrm;
  switch (orientation) {
    case ORIENTATION_HEAD_UP:
    default:
      m_mouse_ebl[ORIENTATION_HEAD_UP] = ebl;
      bearing = ebl;
      break;
    case ORIENTATION_NORTH_UP:
      m_mouse_ebl[ORIENTATION_NORTH_UP] = ebl;
      m_mouse_ebl[ORIENTATION_STABILIZED_UP] = ebl - m_course;
      m_mouse_ebl[ORIENTATION_COG_UP] = ebl - cog;
      bearing = ebl;
      break;
    case ORIENTATION_STABILIZED_UP:
      m_mouse_ebl[ORIENTATION_NORTH_UP] = ebl + m_course;
      m_mouse_ebl[ORIENTATION_COG_UP] = ebl + m_course - cog;
      m_mouse_ebl[ORIENTATION_STABILIZED_UP] = ebl;
      bearing = ebl + m_pi->GetHeadingTrue();
      break;
    case ORIENTATION_COG_UP:
      m_mouse_ebl[ORIENTATION_NORTH_UP] = ebl + cog;
      m_mouse_ebl[ORIENTATION_STABILIZED_UP] = ebl + cog - m_course;
      m_mouse_ebl[ORIENTATION_COG_UP] = ebl;
      bearing = ebl + m_pi->GetHeadingTrue();
      break;
  }

  static double R = 6378.1e3 / 1852.;  // Radius of the Earth in nm
  double brng = deg2rad(bearing);
  double d = vrm;  // Distance in nm

  GeoPosition radar_pos;
  if (GetRadarPosition(&radar_pos)) {
    radar_pos.lat = deg2rad(radar_pos.lat);
    radar_pos.lon = deg2rad(radar_pos.lon);

    double lat2 = asin(sin(radar_pos.lat) * cos(d / R) + cos(radar_pos.lat) * sin(d / R) * cos(brng));
    double lon2 = radar_pos.lon + atan2(sin(brng) * sin(d / R) * cos(radar_pos.lat), cos(d / R) - sin(radar_pos.lat) * sin(lat2));

    m_mouse_pos.lat = rad2deg(lat2);
    m_mouse_pos.lon = rad2deg(lon2);
    LOG_DIALOG(wxT("radar_pi: SetMouseVrmEbl(%f, %f) = %f / %f"), vrm, ebl, m_mouse_pos.lat, m_mouse_pos.lon);
    if (m_control_dialog) {
      m_control_dialog->ShowCursorPane();
    }
  } else {
    m_mouse_pos.lat = nan("");
    m_mouse_pos.lon = nan("");
  }
}

void RadarInfo::SetBearing(int bearing) {
  int orientation = GetOrientation();
  GeoPosition radar_pos;

  if (!isnan(m_vrm[bearing])) {
    m_vrm[bearing] = NAN;
    m_ebl[orientation][bearing] = NAN;
  } else if (!isnan(m_mouse_vrm)) {
    m_vrm[bearing] = m_mouse_vrm;
    for (int i = 0; i < ORIENTATION_NUMBER; i++) {
      m_ebl[i][bearing] = m_mouse_ebl[i];
    }
  } else if (!isnan(m_mouse_pos.lat) && !isnan(m_mouse_pos.lon) && GetRadarPosition(&radar_pos)) {
    m_vrm[bearing] = local_distance(radar_pos, m_mouse_pos);
    m_ebl[orientation][bearing] = local_bearing(radar_pos, m_mouse_pos);
  }
}

void RadarInfo::ComputeTargetTrails() {
  static TrailRevolutionsAge maxRevs[TRAIL_ARRAY_SIZE] = {
      SECONDS_TO_REVOLUTIONS(15),  SECONDS_TO_REVOLUTIONS(30),  SECONDS_TO_REVOLUTIONS(60), SECONDS_TO_REVOLUTIONS(180),
      SECONDS_TO_REVOLUTIONS(300), SECONDS_TO_REVOLUTIONS(600), TRAIL_MAX_REVOLUTIONS + 1};

  int target_trails = m_target_trails.GetValue();
  int trails_motion = m_trails_motion.GetValue();

  TrailRevolutionsAge maxRev = maxRevs[target_trails];
  if (trails_motion == 0) {
    maxRev = 0;
  }
  TrailRevolutionsAge revolution;
  double coloursPerRevolution = 0.;
  double colour = 0.;

  // Like plotter, continuous trails are all very white (non transparent)
  if ((trails_motion > 0) && (target_trails < TRAIL_CONTINUOUS)) {
    coloursPerRevolution = BLOB_HISTORY_COLOURS / (double)maxRev;
  }

  LOG_VERBOSE(wxT("radar_pi: Target trail value %d = %d revolutions"), target_trails, maxRev);

  // Disperse the BLOB_HISTORY values over 0..maxrev
  for (revolution = 0; revolution <= TRAIL_MAX_REVOLUTIONS; revolution++) {
    if (revolution >= 1 && revolution < maxRev) {
      m_trail_colour[revolution] = (BlobColour)(BLOB_HISTORY_0 + (int)colour);
      colour += coloursPerRevolution;
    } else {
      m_trail_colour[revolution] = BLOB_NONE;
    }
    // LOG_VERBOSE(wxT("radar_pi: ComputeTargetTrails rev=%u color=%d"), revolution, m_trail_colour[revolution]);
  }
}

wxString RadarInfo::GetInfoStatus() {
  if (m_receive) {
    return m_receive->GetInfoStatus();
  }
  return _("Uninitialized");
}

void RadarInfo::ClearTrails() {
  if (m_trails) {
    delete m_trails;
  }
  m_trails = new TrailBuffer(this, m_spokes, m_spoke_len_max);
}

int RadarInfo::GetNearestRange(int range_meters, int units) {
  const int *ranges;
  size_t count = RadarFactory::GetRadarRanges(m_radar_type, M_SETTINGS.range_units, &ranges);
  size_t n;

  for (n = count - 1; n > 0; n--) {
    if (ranges[n] <= range_meters) {  // step down until past the right range value
      break;
    }
  }
  return ranges[n];
}

void RadarInfo::AdjustRange(int adjustment) {
  int current_range_meters = m_range.GetValue();
  const int *ranges;
  size_t count = RadarFactory::GetRadarRanges(m_radar_type, M_SETTINGS.range_units, &ranges);
  size_t n;

  m_auto_range_mode = false;
  m_previous_auto_range_meters = 0;

  for (n = count - 1; n > 0; n--) {
    if (ranges[n] <= current_range_meters) {  // step down until past the right range value
      break;
    }
  }

  // Note that we don't actually use m_settings.units here, so that if we are metric and
  // the plotter in NM, and it chose the last range, we start using nautic miles as well.

  if (adjustment < 0 && n > 0) {
    LOG_VERBOSE(wxT("radar_pi: Change radar range from %d to %d"), ranges[n], ranges[n - 1]);
    m_control->SetRange(ranges[n - 1]);
  } else if (adjustment > 0 && n < count - 1) {
    LOG_VERBOSE(wxT("radar_pi: Change radar range from %d to %d"), ranges[n], ranges[n + 1]);
    m_control->SetRange(ranges[n + 1]);
  }
}

wxString RadarInfo::GetTimedIdleText() {
  wxString text;

  if (m_timed_idle.GetValue() > 0) {
    time_t now = time(0);
    int left = m_idle_standby - now;
    if (left >= 0) {
      text = _("Standby in");
      text << wxString::Format(wxT(" %d:%02d"), left / 60, left % 60);
    } else {
      left = m_idle_transmit - now;
      if (left >= 0) {
        text = _("Transmit in");
        text << wxString::Format(wxT(" %d:%02d"), left / 60, left % 60);
      }
    }
  }
  return text;
}

/**
 * See how TimedTransmit is doing.
 *
 * If the ON timer is running and has run out, start the radar and start an OFF timer.
 * If the OFF timer is running and has run out, stop the radar and start an ON timer.
 */
void RadarInfo::CheckTimedTransmit() {
  if (m_timed_idle.GetValue() == 0) {
    return;  // User does not want timed idle
  }

  RadarState state = (RadarState)m_state.GetValue();
  if (state == RADAR_OFF) {
    return;  // Timers are just stuck at existing value if radar is off.
  }

  time_t now = time(0);

  if (m_idle_standby > 0 && TIMED_OUT(now, m_idle_standby) && state == RADAR_TRANSMIT) {
    RequestRadarState(RADAR_STANDBY);
    m_idle_transmit = now + m_timed_idle.GetValue() * SECONDS_PER_TIMED_IDLE_SETTING -
    (m_timed_run.GetValue() + 1) * SECONDS_PER_TIMED_RUN_SETTING;
    m_idle_standby = 0;
  } else if (m_idle_transmit > 0 && TIMED_OUT(now, m_idle_transmit) && state == RADAR_STANDBY) {
    RequestRadarState(RADAR_TRANSMIT);
    m_idle_standby = now + (m_timed_run.GetValue() + 1) * SECONDS_PER_TIMED_RUN_SETTING;
    m_idle_transmit = 0;
  }
}

PLUGIN_END_NAMESPACE
