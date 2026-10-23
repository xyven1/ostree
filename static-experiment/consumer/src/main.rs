//! Smoke test for a Rust binary with libostree linked in statically.
//!
//! Usage: ostree-static-consumer [-b|--branch <ref>] <workdir> [remote-url]
//!
//! Creates <workdir>/src-repo (archive) with one commit of a small tree on
//! <ref> (default static/test), pulls <ref> into <workdir>/dst-repo from
//! `remote-url` (default: file:// of src-repo; pass an http:// URL to
//! exercise the curl fetcher), then checks it out. When the pulled commit is
//! the one just made, a file's contents are verified too.

use std::path::{Path, PathBuf};

use anyhow::{bail, ensure, Context, Result};
use gio::prelude::*;

const DEFAULT_REF: &str = "static/test";
const USAGE: &str = "usage: ostree-static-consumer [-b|--branch <ref>] <workdir> [remote-url]";

fn write_tree(dir: &Path) -> Result<()> {
    std::fs::create_dir_all(dir.join("etc"))?;
    std::fs::write(dir.join("etc/hello"), "hello from a static libostree\n")?;
    std::fs::write(dir.join("big"), vec![b'x'; 1 << 20])?;
    std::os::unix::fs::symlink("etc/hello", dir.join("link"))?;
    Ok(())
}

fn commit(repo: &ostree::Repo, tree: &Path, branch: &str) -> Result<String> {
    let c = gio::Cancellable::NONE;
    repo.prepare_transaction(c)?;
    let mtree = ostree::MutableTree::new();
    let modifier = ostree::RepoCommitModifier::new(
        ostree::RepoCommitModifierFlags::CANONICAL_PERMISSIONS,
        None,
    );
    repo.write_directory_to_mtree(&gio::File::for_path(tree), &mtree, Some(&modifier), c)?;
    let root = repo.write_mtree(&mtree, c)?;
    let root = root.downcast::<ostree::RepoFile>().unwrap();
    let csum = repo.write_commit(None, Some("static test"), None, None, &root, c)?;
    repo.transaction_set_ref(None, branch, Some(&csum));
    repo.commit_transaction(c)?;
    Ok(csum.to_string())
}

fn main() -> Result<()> {
    let mut branch = DEFAULT_REF.to_string();
    let mut positional = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-b" | "--branch" => branch = args.next().context(USAGE)?,
            "-h" | "--help" => {
                println!("{USAGE}");
                return Ok(());
            }
            _ if arg.starts_with("--branch=") => branch = arg["--branch=".len()..].to_string(),
            _ if arg.starts_with('-') => bail!("unknown option {arg}\n{USAGE}"),
            _ => positional.push(arg),
        }
    }
    ensure!(!branch.is_empty(), "empty branch\n{USAGE}");
    let mut positional = positional.into_iter();
    let work = PathBuf::from(positional.next().context(USAGE)?);
    ensure!(positional.len() <= 1, "too many arguments\n{USAGE}");
    std::fs::create_dir_all(&work)?;
    let c = gio::Cancellable::NONE;

    println!("libostree >= 2025.3: {}", ostree::check_version(2025, 3));

    let src = ostree::Repo::new_for_path(work.join("src-repo"));
    src.create(ostree::RepoMode::Archive, c)?;
    let tree = work.join("tree");
    write_tree(&tree)?;
    let csum = commit(&src, &tree, &branch)?;
    println!("committed {branch} = {csum}");

    // With an explicit URL the remote holds some other commit.
    let explicit_url = positional.next();
    let url = explicit_url
        .clone()
        .unwrap_or_else(|| format!("file://{}", work.join("src-repo").canonicalize().unwrap().display()));
    let dst = ostree::Repo::new_for_path(work.join("dst-repo"));
    dst.create(ostree::RepoMode::BareUser, c)?;
    let opts = glib::VariantDict::new(None);
    opts.insert("gpg-verify", false);
    dst.remote_add("origin", Some(&url), Some(&opts.end()), c)?;
    dst.pull("origin", &[branch.as_str()], ostree::RepoPullFlags::NONE, None, c)
        .with_context(|| format!("pull {branch} from {url}"))?;
    let pulled = dst
        .resolve_rev(&format!("origin:{branch}"), false)?
        .context("ref missing after pull")?;
    ensure!(explicit_url.is_some() || pulled == csum, "pulled {pulled}, expected {csum}");
    println!("pulled {branch} = {pulled} from {url}");

    let co = work.join("checkout");
    let mut copts = ostree::RepoCheckoutAtOptions::default();
    copts.mode = ostree::RepoCheckoutMode::User;
    dst.checkout_at(Some(&copts), libc::AT_FDCWD, &co, &pulled, c)?;
    if pulled == csum {
        let got = std::fs::read_to_string(co.join("link"))?;
        ensure!(got == "hello from a static libostree\n", "bad checkout: {got:?}");
    } else {
        let mut top: Vec<_> = std::fs::read_dir(&co)?
            .map(|e| e.map(|e| e.file_name().to_string_lossy().into_owned()))
            .collect::<std::io::Result<_>>()?;
        top.sort();
        println!("top level: {}", top.join(" "));
    }
    println!("checkout OK: {}", co.display());
    Ok(())
}
