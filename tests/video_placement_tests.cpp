#include "cyvr/player/application.hpp"
#include "cyvr/player/video_placement.hpp"
#include <iostream>
#include <stdexcept>

using namespace cyvr;
using namespace cyvr::player;
namespace {
int checks{};
void require(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }
bool same(Vec3 a, Vec3 b) {
  return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}
bool same(Quat a, Quat b) {
  return std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) > 1 - 1e-5f;
}
bool same(Pose a, Pose b) {
  return same(a.position, b.position) && same(a.orientation, b.orientation);
}
} // namespace
int main() {
  try {
    const Pose head{{0, 1.6f, 0}, {}};
    auto panel = placePanel(head, 1.35f, 1);
    VideoPlacement video;
    ImageSettings image;
    require(!video.placed(), "A new scene must await video placement");
    video.place(head, 1.35f);
    const auto original = video.screen(image);
    require(
        same(original.position, {0, 1.6f, -3.5f}),
        "Initial cinema placement must retain the existing viewing distance");
    require(same(original.orientation, head.orientation),
            "Initial video orientation must face the viewer");
    {
      SceneSettings scene;scene.x=2;scene.y=-.75f;scene.z=1;scene.yaw=90;scene.pitch=-35;scene.roll=18;
      const auto moved=video.screen(image,CinemaSettings{},scene);
      require(same(moved.position,original.position+Vec3{2,-.75f,1}),"Six-axis placement must move the video in its own anchor axes");
      require(!same(moved.orientation,original.orientation),"Explicit video rotation was ignored");
      const auto panelBefore=panel.pose;
      const Pose change{{.4f,.3f,-1},yawRotation(.8f)};
      video.transform(inverse_pose(change));
      require(same(compose_pose(change,video.screen(image,CinemaSettings{},scene)),moved),"Tracking origin rebasing must preserve manually placed and rotated video");
      require(same(panel.pose,panelBefore),"Manual video placement must not move the controls");
      video.transform(change);
      scene.x=NAN;scene.yaw=999;scene.sanitize();require(scene.x==0 && scene.yaw==180,"Invalid placement must be bounded");
    }
    CinemaSettings cinema;cinema.width=7;cinema.distance=6;cinema.elevation=.75f;cinema.curvature=90;cinema.pitch=25;cinema.roll=-12;
    const auto customized=video.screen(image,cinema);
    require(same(customized.position,{0,2.35f,-6}),"Screen distance and height must be independent of the controls");
    const auto center=cylinder_center(customized,cinema.radius());
    const auto middle=center.position+rotate(center.orientation,{0,0,-cinema.radius()});
    require(same(middle,customized.position),"Cylinder center must retain the flat screen's midpoint after tilt/roll");
    require(near(cinema.radius()*cinema.angle(),cinema.width),"Curving the screen must preserve its arc width and texture aspect");
    video.place(head,2);
    require(same(video.screen(image,cinema),customized),"UI distance must not change explicit movie distance");
    video.place(head,1.35f);
    cinema.environment=7;cinema.ambient=.12f;cinema.panel_opacity=.6f;cinema.reset_screen();
    require(cinema.width==4 && cinema.curvature==0 && cinema.environment==7 && cinema.ambient==.12f && cinema.panel_opacity==.6f,"Reset screen must preserve environment settings");
    for (int i = 0; i < 120; ++i) {
      // Realistic UI drag, release-facing rotation, resize/follow and
      // hide/wake.
      panel.pose.position = {float(i) * .013f, 1.2f, -1.1f};
      panel.pose.orientation = faceViewer(panel.pose.position, head.position);
      panel.width = 1.2f * (1 + i * .001f);
      require(same(video.screen(image), original),
              "Dragging/releasing UI must not change the video's pose");
    }
    image.zoom = 3;
    require(same(video.screen(image), original),
            "Zoom must change screen size without moving its scene anchor");
    image.pan_x = .4f;
    image.pan_y = -.2f;
    require(same(video.screen(image).position,
                 original.position + Vec3{.8f, .4f, 0}),
            "Explicit video position controls must remain functional");
    image = ImageSettings{};
    const Pose new_origin_in_previous{{1, .2f, -.3f}, yawRotation(.7f)};
    video.transform(inverse_pose(new_origin_in_previous));
    require(
        same(compose_pose(new_origin_in_previous, video.screen(image)),
             original),
        "A known OpenXR origin change must preserve physical video placement");
    video.reset();
    require(!video.placed(),
            "A new XR scene must discard old coordinate-space poses");
    // Initial tracking fallback is VIEW relative; retain the pose when LOCAL
    // becomes valid rather than inheriting the host's newly positioned UI.
    video.place(Pose{}, 1.35f);
    const auto in_view = video.screen(image);
    const Pose local_from_view{{2, 1.7f, 1}, yawRotation(-.9f)};
    video.transform(local_from_view);
    require(
        same(video.screen(image), compose_pose(local_from_view, in_view)),
        "VIEW-to-LOCAL transition must transform the existing video anchor");
    const auto moved_panel = panel.pose;
    {
      const Quat tilted{std::sin(.6f),0,0,std::cos(.6f)};
      const Pose reclining{{.5f,1.2f,.3f},tilted};
      video.place(reclining,1.35f,true);
      const auto centered=video.screen(image);
      require(same(centered.orientation,tilted) && same(centered.position,reclining.position+rotate(tilted,{0,0,-3.5f})),"Place in front must honor the actual tilted headset view");
    }
    const Pose new_head{{-.3f, 1.8f, .4f}, yawRotation(1.1f)};
    video.place(new_head, 1.35f);
    require(!same(video.screen(image), original),
            "Explicit video recenter must adopt the current view");
    require(same(panel.pose, moved_panel),
            "Recentering video must not reposition the controls");
    // The UI command targets only the application-owned anchor. The host's
    // generic recenter flag would move the controls as well.
    {
      UiModel ui;
      ui.presentation = Presentation::Immersive;
      ui.recenterRequested = false;
      Platform platform;
      platform.root = ".";
      PlayerApplication app(ui, platform);
      const auto before = app.video_placement_request;
      require(app.action(1050), "Video recenter command must be handled");
      require(app.video_placement_request == before + 1,
              "Video recenter must notify the video renderer");
      require(!ui.recenterRequested,
              "Video recenter must not request a panel recenter");
    }
    std::cout << "Video placement: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
