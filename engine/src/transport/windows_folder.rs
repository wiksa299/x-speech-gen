use anyhow::{Result, ensure};
use std::{ffi::OsString, os::windows::ffi::OsStringExt, path::PathBuf, ptr::null};
use windows_sys::Win32::{
    System::Com::{
        COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE, CoInitializeEx, CoTaskMemFree,
        CoUninitialize,
    },
    UI::Shell::{
        BIF_EDITBOX, BIF_NEWDIALOGSTYLE, BIF_NONEWFOLDERBUTTON, BIF_RETURNONLYFSDIRS, BROWSEINFOW,
        SHBrowseForFolderW, SHGetPathFromIDListEx,
    },
};

pub(super) fn browse() -> Result<Option<PathBuf>> {
    // The engine thread owns the apartment and the modal picker for its entire lifetime.
    let status = unsafe {
        CoInitializeEx(
            null(),
            (COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE) as u32,
        )
    };
    ensure!(status >= 0, crate::locale::message("error.picker"));
    struct Apartment;
    impl Drop for Apartment {
        fn drop(&mut self) {
            // CoInitializeEx succeeded on this thread.
            unsafe { CoUninitialize() }
        }
    }
    let _apartment = Apartment;
    let mut name = [0_u16; 260];
    let title: Vec<u16> = crate::locale::message("folder.picker")
        .to_string()
        .encode_utf16()
        .chain(Some(0))
        .collect();
    let info = BROWSEINFOW {
        pszDisplayName: name.as_mut_ptr(),
        lpszTitle: title.as_ptr(),
        ulFlags: BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX | BIF_NONEWFOLDERBUTTON,
        ..Default::default()
    };
    // The shell writes the display name into the live MAX_PATH buffer in info.
    let item = unsafe { SHBrowseForFolderW(&info) };
    if item.is_null() {
        return Ok(None);
    }
    let mut path = vec![0_u16; 32768];
    // Both buffers stay valid through the call. The shell owns the item until CoTaskMemFree.
    let found = unsafe {
        let found = SHGetPathFromIDListEx(item, path.as_mut_ptr(), path.len() as u32, 0);
        CoTaskMemFree(item.cast());
        found
    };
    ensure!(found != 0, crate::locale::message("error.picker_path"));
    let end = path
        .iter()
        .position(|&character| character == 0)
        .unwrap_or(path.len());
    Ok(Some(PathBuf::from(OsString::from_wide(&path[..end]))))
}
