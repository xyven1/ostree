//! Smoke test for a Rust binary with libostree linked in statically.
//!
//! Usage: ostree-static-consumer [--ref <ref>] <workdir> [remote-url]
//!
//! With `remote-url`, pulls <ref> into <workdir>/dst-repo and checks it out.
//! <ref> is passed to the pull as given, so it may be a branch or a commit
//! checksum. Ref binding verification is disabled, so commits whose binding
//! metadata does not list <ref> still pull.
//!
//! Without `remote-url`, first creates <workdir>/src-repo (archive) with one
//! commit of a small tree on <ref> (default static/test), pulls that over
//! file://, and verifies a file's contents after checkout.

use std::path::{Path, PathBuf};

use anyhow::{bail, ensure, Context, Result};
use gio::prelude::*;

const DEFAULT_REF: &str = "static/test";
const USAGE: &str = "usage: ostree-static-consumer [--ref <ref>] <workdir> [remote-url]";

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
    let mut rev = DEFAULT_REF.to_string();
    let mut positional = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--ref" => rev = args.next().context(USAGE)?,
            "-h" | "--help" => {
                println!("{USAGE}");
                return Ok(());
            }
            _ if arg.starts_with("--ref=") => rev = arg["--ref=".len()..].to_string(),
            _ if arg.starts_with('-') => bail!("unknown option {arg}\n{USAGE}"),
            _ => positional.push(arg),
        }
    }
    ensure!(!rev.is_empty(), "empty ref\n{USAGE}");
    let mut positional = positional.into_iter();
    let work = PathBuf::from(positional.next().context(USAGE)?);
    let url = positional.next();
    ensure!(positional.len() == 0, "too many arguments\n{USAGE}");
    std::fs::create_dir_all(&work)?;
    let c = gio::Cancellable::NONE;
    let is_checksum = ostree::validate_checksum_string(&rev).is_ok();

    println!("libostree >= 2025.3: {}", ostree::check_version(2025, 3));

    // Without a URL, make something to pull.
    let (url, own_commit) = match url {
        Some(url) => (url, None),
        None => {
            ensure!(!is_checksum, "--ref must be a branch name when no remote-url is given");
            let src = ostree::Repo::new_for_path(work.join("src-repo"));
            src.create(ostree::RepoMode::Archive, c)?;
            let tree = work.join("tree");
            write_tree(&tree)?;
            let csum = commit(&src, &tree, &rev)?;
            println!("committed {rev} = {csum}");
            let path = work.join("src-repo").canonicalize()?;
            (format!("file://{}", path.display()), Some(csum))
        }
    };

    let dst = ostree::Repo::new_for_path(work.join("dst-repo"));
    dst.create(ostree::RepoMode::BareUser, c)?;
    let opts = glib::VariantDict::new(None);
    opts.insert("gpg-verify", false);
    dst.remote_add("origin", Some(&url), Some(&opts.end()), c)?;

    let pull_opts = glib::VariantDict::new(None);
    pull_opts.insert("refs", vec![rev.clone()]);
    pull_opts.insert("disable-verify-bindings", true);
    dst.pull_with_options("origin", &pull_opts.end(), None, c)
        .with_context(|| format!("pull {rev} from {url}"))?;
    // A pulled checksum leaves no ref behind; a branch lands under origin:.
    let pulled = if is_checksum {
        rev.clone()
    } else {
        dst.resolve_rev(&format!("origin:{rev}"), false)?
            .context("ref missing after pull")?
            .to_string()
    };
    if let Some(csum) = &own_commit {
        ensure!(&pulled == csum, "pulled {pulled}, expected {csum}");
    }
    println!("pulled {rev} = {pulled} from {url}");

    let co = work.join("checkout");
    let mut copts = ostree::RepoCheckoutAtOptions::default();
    copts.mode = ostree::RepoCheckoutMode::User;
    dst.checkout_at(Some(&copts), libc::AT_FDCWD, &co, &pulled, c)?;
    if own_commit.is_some() {
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
