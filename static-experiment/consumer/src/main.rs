//! Smoke test for a Rust binary with libostree linked in statically.
//!
//! Usage: ostree-static-consumer <workdir> [remote-url]
//!
//! Creates <workdir>/src-repo (archive) with one commit of a small tree,
//! pulls it into <workdir>/dst-repo from `remote-url` (default: file:// of
//! src-repo; pass an http:// URL serving src-repo to exercise the curl
//! fetcher), then checks the commit out and verifies a file's contents.

use std::path::{Path, PathBuf};

use anyhow::{ensure, Context, Result};
use gio::prelude::*;

const REF: &str = "static/test";

fn write_tree(dir: &Path) -> Result<()> {
    std::fs::create_dir_all(dir.join("etc"))?;
    std::fs::write(dir.join("etc/hello"), "hello from a static libostree\n")?;
    std::fs::write(dir.join("big"), vec![b'x'; 1 << 20])?;
    std::os::unix::fs::symlink("etc/hello", dir.join("link"))?;
    Ok(())
}

fn commit(repo: &ostree::Repo, tree: &Path) -> Result<String> {
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
    repo.transaction_set_ref(None, REF, Some(&csum));
    repo.commit_transaction(c)?;
    Ok(csum.to_string())
}

fn main() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let work = PathBuf::from(args.next().context("usage: <workdir> [remote-url]")?);
    std::fs::create_dir_all(&work)?;
    let c = gio::Cancellable::NONE;

    println!("libostree >= 2025.3: {}", ostree::check_version(2025, 3));

    let src = ostree::Repo::new_for_path(work.join("src-repo"));
    src.create(ostree::RepoMode::Archive, c)?;
    let tree = work.join("tree");
    write_tree(&tree)?;
    let csum = commit(&src, &tree)?;
    println!("committed {REF} = {csum}");

    // With an explicit URL the remote holds some other run's commit.
    let explicit_url = args.next();
    let url = explicit_url
        .clone()
        .unwrap_or_else(|| format!("file://{}", work.join("src-repo").canonicalize().unwrap().display()));
    let dst = ostree::Repo::new_for_path(work.join("dst-repo"));
    dst.create(ostree::RepoMode::BareUser, c)?;
    let opts = glib::VariantDict::new(None);
    opts.insert("gpg-verify", false);
    dst.remote_add("origin", Some(&url), Some(&opts.end()), c)?;
    dst.pull("origin", &[REF], ostree::RepoPullFlags::NONE, None, c)
        .with_context(|| format!("pull from {url}"))?;
    let pulled = dst
        .resolve_rev(&format!("origin:{REF}"), false)?
        .context("ref missing after pull")?;
    ensure!(explicit_url.is_some() || pulled == csum, "pulled {pulled}, expected {csum}");
    println!("pulled {pulled} from {url}");

    let co = work.join("checkout");
    let mut copts = ostree::RepoCheckoutAtOptions::default();
    copts.mode = ostree::RepoCheckoutMode::User;
    dst.checkout_at(Some(&copts), libc::AT_FDCWD, &co, &pulled, c)?;
    let got = std::fs::read_to_string(co.join("link"))?;
    ensure!(got == "hello from a static libostree\n", "bad checkout: {got:?}");
    println!("checkout OK: {}", co.display());
    Ok(())
}
