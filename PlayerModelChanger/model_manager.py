"""Offline player appearance preset manager for the Triad Wars local server.

Only uses game-shipped clothing/outfits; it does not import meshes or change game files.
"""
import copy
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SERVER = ROOT / 'LocalServer'
STATE = SERVER / 'state.json'
BACKUP = SERVER / 'player-model-backup.json'
sys.path.insert(0, str(SERVER))
sys.path.insert(0, str(SERVER / 'tools'))
import state
import playermodel


def guard_offline():
    with socket.socket() as s:
        s.settimeout(0.4)
        if s.connect_ex(('127.0.0.1', 443)) == 0:
            raise RuntimeError('Port 443 is in use. Close the local server and game first.')
    if os.name == 'nt':
        try:
            result = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq TriadWars.exe', '/NH'],
                                    capture_output=True, text=True, timeout=5, check=True,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        except (OSError, subprocess.SubprocessError) as exc:
            raise RuntimeError('Could not check if TriadWars.exe is running. Close it and retry.') from exc
        if 'triadwars.exe' in result.stdout.lower():
            raise RuntimeError('TriadWars.exe is running. Close the game and server first.')


def read_json(path):
    with open(path, encoding='utf-8-sig') as f:
        return json.load(f)


def atomic_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = None
    try:
        with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=path.parent,
                                         prefix='.player-model-', delete=False) as f:
            tmp = f.name
            json.dump(data, f, indent=1, ensure_ascii=False)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        tmp = None
    finally:
        if tmp and os.path.exists(tmp):
            os.unlink(tmp)


def account(player):
    value = player.get('PlayerDataBag', {}).get('AccountID')
    if value is None:
        raise ValueError('Profile has no account ID; no changes made.')
    player_id = player.get('PlayerDataBag', {}).get('PlayerDataID')
    if player_id is None:
        raise ValueError('Profile has no player ID; no changes made.')
    return '%s:%s' % (value, player_id)


def catalogue(mods_dir):
    """Return (valid entries, errors); invalid mods cannot be applied."""
    looks = {item['id']: item for item in playermodel.looks()}
    outfits = set(playermodel.catalogue()['items'])
    entries = [{'name': item['name'], 'kind': 'look', 'id': item['id'],
                'detail': '%s — %s' % (item['theme'], playermodel.lookbook.describe(item))}
               for item in looks.values()]
    names = playermodel.names()
    entries.extend({'name': names.get(uid, uid), 'kind': 'outfit', 'id': uid,
                    'detail': 'Built-in game outfit — ' + uid}
                   for uid in playermodel.catalogue()['items'])
    errors = []
    mods_dir = Path(mods_dir)
    if mods_dir.exists():
        for folder in sorted(mods_dir.iterdir()):
            if not folder.is_dir():
                continue
            path = folder / 'model.json'
            if not path.is_file():
                errors.append('%s: missing model.json' % folder.name)
                continue
            try:
                obj = read_json(path)
                if not isinstance(obj, dict) or set(obj) - {'name', 'look', 'outfit'}:
                    raise ValueError('model.json must contain name and one look or outfit')
                name = obj.get('name')
                if not isinstance(name, str) or not name.strip() or len(name) > 100:
                    raise ValueError('name must be 1–100 characters')
                if ('look' in obj) == ('outfit' in obj):
                    raise ValueError('specify exactly one of look or outfit')
                kind = 'look' if 'look' in obj else 'outfit'
                uid = obj[kind]
                if not isinstance(uid, str) or uid not in (looks if kind == 'look' else outfits):
                    raise ValueError('unknown %s ID; use a game-shipped ID' % kind)
                entries.append({'name': name.strip(), 'kind': kind, 'id': uid,
                                'detail': 'Preset folder: %s (%s %s)' % (folder.name, kind, uid)})
            except (ValueError, OSError, json.JSONDecodeError) as exc:
                errors.append('%s: %s' % (folder.name, exc))
    return entries, errors


def get_player(path):
    data = read_json(path)
    player = data.get('player') if isinstance(data, dict) else None
    if not isinstance(player, dict):
        raise ValueError('Profile has no player; start the local server and game once first.')
    account(player)
    return data, player


def apply(entry, state_path=STATE, backup_path=BACKUP, guard=guard_offline):
    guard()
    data, player = get_player(state_path)
    # Resolve again: UI entries or files can be changed after the list was drawn.
    kind, uid = entry['kind'], entry['id']
    if kind == 'look':
        match = next((x for x in playermodel.looks() if x['id'] == uid), None)
        if not match:
            raise ValueError('Unknown look; no changes made.')
    elif kind == 'outfit':
        if uid not in playermodel.catalogue()['items'] or not playermodel.catalogue().get('pieces', {}).get(uid):
            raise ValueError('Unknown or incomplete outfit; no changes made.')
    else:
        raise ValueError('Unknown preset type; no changes made.')
    # A first backup survives further changes, and is not silently replaced.
    backup_path = Path(backup_path)
    if backup_path.exists():
        backup = read_json(backup_path)
        if str(backup.get('account')) != account(player) or not isinstance(backup.get('customization'), dict):
            raise ValueError('Existing backup belongs to another profile or is invalid; no changes made.')
    else:
        atomic_json(backup_path, {'account': account(player),
                                  'customization': copy.deepcopy(player.get('CustomizationBag', state.new_customization_bag()))})
    if kind == 'look':
        playermodel.wear_look(player, match)
    else:
        playermodel.wear(player, uid)
    atomic_json(state_path, data)


def restore(state_path=STATE, backup_path=BACKUP, guard=guard_offline):
    guard()
    data, player = get_player(state_path)
    backup = read_json(backup_path)
    if str(backup.get('account')) != account(player) or not isinstance(backup.get('customization'), dict):
        raise ValueError('Backup is invalid or belongs to another profile; no changes made.')
    player['CustomizationBag'] = backup['customization']
    atomic_json(state_path, data)
    # Keep the backup: it can be restored again without deleting progress or inventory.


def main():
    import tkinter as tk
    from tkinter import messagebox, ttk
    app = tk.Tk()
    app.title('Triad Wars Player Model Changer — built-in looks')
    app.geometry('760x540')
    ttk.Label(app, text='PLAYER MODEL CHANGER', font=('Segoe UI', 16, 'bold')).pack(pady=(12, 2))
    ttk.Label(app, text='Built-in looks and outfit presets only • Close the game and local server before applying').pack()
    frame = ttk.Frame(app, padding=12); frame.pack(fill='both', expand=True)
    listing = tk.Listbox(frame, font=('Segoe UI', 11), exportselection=False)
    scroll = ttk.Scrollbar(frame, orient='vertical', command=listing.yview)
    listing.configure(yscrollcommand=scroll.set); listing.pack(side='left', fill='both', expand=True); scroll.pack(side='right', fill='y')
    detail = tk.StringVar(value='Select a look. Folders in Mods can add presets, not new meshes.')
    ttk.Label(app, textvariable=detail, wraplength=720).pack(padx=12, pady=5)
    entries = []
    def refresh():
        nonlocal entries
        try:
            entries, errors = catalogue(Path(__file__).parent / 'Mods')
            listing.delete(0, 'end')
            for entry in entries: listing.insert('end', entry['name'])
            detail.set('%d choices. %d invalid preset folder(s).' % (len(entries), len(errors)))
            if errors: messagebox.showwarning('Preset errors', '\n'.join(errors[:20]))
        except Exception as exc: messagebox.showerror('Cannot load catalogue', str(exc))
    def selected(_event=None):
        sel = listing.curselection()
        if sel: detail.set(entries[sel[0]]['detail'])
    listing.bind('<<ListboxSelect>>', selected)
    def action_restore():
        if not messagebox.askyesno('Restore', 'Restore the appearance saved before your first change? Inventory and other progress are kept.'):
            return
        try: restore(); messagebox.showinfo('Restored', 'Appearance restored. Start the game again to see it.')
        except Exception as exc: messagebox.showerror('Could not restore', str(exc))
    def action_apply():
        sel = listing.curselection()
        if not sel: messagebox.showwarning('Select a look', 'Choose a look first.'); return
        entry = entries[sel[0]]
        if not messagebox.askyesno('Apply look', 'Wear %s? The game and local server must be closed.' % entry['name']): return
        try: apply(entry); messagebox.showinfo('Applied', 'Saved %s. Start the game again to see it.' % entry['name'])
        except Exception as exc: messagebox.showerror('Could not apply', str(exc))
    buttons = ttk.Frame(app, padding=(12, 4, 12, 12)); buttons.pack(fill='x')
    ttk.Button(buttons, text='Apply selected', command=action_apply).pack(side='left', padx=3)
    ttk.Button(buttons, text='Restore original appearance', command=action_restore).pack(side='left', padx=3)
    ttk.Button(buttons, text='Rescan preset folders', command=refresh).pack(side='left', padx=3)
    ttk.Button(buttons, text='Close', command=app.destroy).pack(side='right', padx=3)
    refresh(); app.mainloop()

if __name__ == '__main__':
    main()
