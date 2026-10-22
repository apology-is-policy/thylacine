struct Comp {conn_seq:u64,gen_seq:u32}
impl Comp {
    pub fn next_conn_id(&mut self) -> Option<u64> {
        // Connection identities outlive individual fids and must never repeat.
        self.conn_seq = self.conn_seq.checked_add(1)?;
        Some(self.conn_seq)
    }
fn next_gen(&mut self)->Option<()> {
self.gen_seq = self.gen_seq.checked_add(1)?;
Some(())}}

#[test] fn exhaustion_never_reuses_identity() {
 let mut c=Comp{conn_seq:u64::MAX-1,gen_seq:u32::MAX-1};
 assert_eq!(c.next_conn_id(),Some(u64::MAX));
 assert_eq!(c.next_conn_id(),None);assert_eq!(c.next_conn_id(),None);
 assert_eq!(c.next_gen(),Some(()));assert_eq!(c.gen_seq,u32::MAX);
 assert_eq!(c.next_gen(),None);assert_eq!(c.next_gen(),None);
 assert_eq!(c.gen_seq,u32::MAX);
}
