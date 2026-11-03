//! ext-workspace from the compositor's side: workspaces with ids, coordinates
//! and state bits reach a pager only at `commit`, and a removed group goes.

#[macro_use]
mod common;

use common::{ClientSide, Peer};
use luminaria::core::Display;
use luminaria::desktop::{WorkspaceManager, workspace_state};
use std::sync::{Arc, Mutex};
use wayland_client::globals::registry_queue_init;
use wayland_client::{Connection, Dispatch, QueueHandle, event_created_child};
use wayland_protocols::ext::workspace::v1::client::{
    ext_workspace_group_handle_v1 as group, ext_workspace_handle_v1 as handle,
    ext_workspace_manager_v1 as manager,
};

#[derive(Default, Clone, Debug, PartialEq)]
struct Seen {
    id: String,
    name: String,
    coordinates: Vec<u8>,
    state: u32,
    removed: bool,
}

#[derive(Default)]
struct State {
    groups: usize,
    groups_removed: usize,
    workspaces: Vec<(handle::ExtWorkspaceHandleV1, Seen)>,
}

registry_state!(State);

impl Dispatch<manager::ExtWorkspaceManagerV1, ()> for State {
    fn event(
        _: &mut Self,
        _: &manager::ExtWorkspaceManagerV1,
        _: manager::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
    event_created_child!(State, manager::ExtWorkspaceManagerV1, [
        manager::EVT_WORKSPACE_GROUP_OPCODE => (group::ExtWorkspaceGroupHandleV1, ()),
        manager::EVT_WORKSPACE_OPCODE => (handle::ExtWorkspaceHandleV1, ()),
    ]);
}

impl Dispatch<group::ExtWorkspaceGroupHandleV1, ()> for State {
    fn event(
        state: &mut Self,
        _: &group::ExtWorkspaceGroupHandleV1,
        event: group::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            group::Event::Capabilities { .. } => state.groups += 1,
            group::Event::Removed => state.groups_removed += 1,
            _ => {}
        }
    }
}

impl Dispatch<handle::ExtWorkspaceHandleV1, ()> for State {
    fn event(
        state: &mut Self,
        proxy: &handle::ExtWorkspaceHandleV1,
        event: handle::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let index = match state.workspaces.iter().position(|(p, _)| p == proxy) {
            Some(index) => index,
            None => {
                state.workspaces.push((proxy.clone(), Seen::default()));
                state.workspaces.len() - 1
            }
        };
        let seen = &mut state.workspaces[index].1;
        match event {
            handle::Event::Id { id } => seen.id = id,
            handle::Event::Name { name } => seen.name = name,
            handle::Event::Coordinates { coordinates } => seen.coordinates = coordinates,
            handle::Event::State {
                state: wayland_client::WEnum::Value(bits),
            } => seen.state = bits.bits(),
            handle::Event::Removed => seen.removed = true,
            _ => {}
        }
    }
}

type Shared = Arc<Mutex<Vec<Vec<Seen>>>>;

fn pager(seen: Shared) -> impl FnOnce(Connection, ClientSide) {
    move |conn, step| {
        let (globals, mut queue) = registry_queue_init::<State>(&conn).expect("registry");
        let qh = queue.handle();
        let _manager: manager::ExtWorkspaceManagerV1 = globals.bind(&qh, 1..=1, ()).unwrap();
        let mut state = State::default();
        queue.roundtrip(&mut state).unwrap();
        assert_eq!((state.groups, state.groups_removed), (1, 0));
        seen.lock()
            .unwrap()
            .push(state.workspaces.iter().map(|(_, s)| s.clone()).collect());
        step.step("listed");
        queue.roundtrip(&mut state).unwrap();
        assert_eq!(state.groups_removed, 1);
        seen.lock()
            .unwrap()
            .push(state.workspaces.iter().map(|(_, s)| s.clone()).collect());
        step.step("changed");
    }
}

#[test]
fn a_pager_sees_ids_coordinates_and_states_at_commit() {
    let display = Display::new().expect("display");
    let mut workspaces = WorkspaceManager::new(&display).expect("workspaces");
    let group = workspaces.create_group();
    let first = workspaces.add_workspace(group, "pastel-1", "1", &[0]);
    let second = workspaces.add_workspace(group, "pastel-2", "2", &[1]);
    workspaces.activate_workspace(first);
    workspaces.set_state(second, workspace_state::URGENT);
    workspaces.commit();

    let seen: Shared = Arc::default();
    let mut peer = Peer::spawn(&display, pager(seen.clone()));
    peer.wait(&display, "listed");
    {
        let listed = &seen.lock().unwrap()[0];
        assert_eq!(listed.len(), 2);
        assert_eq!(
            (listed[0].id.as_str(), listed[0].name.as_str()),
            ("pastel-1", "1")
        );
        assert_eq!(listed[1].coordinates, 1i32.to_ne_bytes());
        assert_eq!(listed[0].state, workspace_state::ACTIVE);
        assert_eq!(listed[1].state, workspace_state::URGENT);
    }

    workspaces.activate_workspace(second);
    workspaces.remove_workspace(first);
    workspaces.remove_group(group);
    workspaces.commit();
    peer.go();
    peer.wait(&display, "changed");
    {
        let changed = &seen.lock().unwrap()[1];
        assert!(changed[0].removed);
        assert_eq!(
            changed[1].state,
            workspace_state::ACTIVE | workspace_state::URGENT
        );
    }
    peer.finish(&display);
}
